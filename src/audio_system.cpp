// audio_system.cpp — WASAPI + Media Foundation audio playback
// Architecture:
//   AudioThread: đọc MF audio samples → convert → push vào WASAPI render buffer
//   Volume/Mute: ISimpleAudioVolume (per-session, không ảnh hưởng system volume)
//   Sync với video: dùng cùng wall clock anchor, không cần lock chéo với decode thread

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include "audio_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

using namespace std;

// ============================================================
//  COM helpers
// ============================================================
template<typename T> static inline void SafeRel(T*& p) {
    if (p) { p->Release(); p = NULL; }
}

// ============================================================
//  Internal state
// ============================================================
static HANDLE               g_hAudioThread   = NULL;
static volatile bool        g_audioRunning   = false;
static volatile bool        g_audioPaused    = false;
static volatile bool        g_audioMuted     = false;
static volatile float       g_audioVolume    = 1.0f;
static volatile LONGLONG    g_audioSeekPos   = -1LL; // -1 = no pending seek

static IMMDeviceEnumerator* g_pEnum          = NULL;
static IMMDevice*           g_pDevice        = NULL;
static IAudioClient*        g_pAudioClient   = NULL;
static IAudioRenderClient*  g_pRenderClient  = NULL;
static ISimpleAudioVolume*  g_pSimpleVol     = NULL;
static IMFSourceReader*     g_pAudioReader   = NULL;

static WAVEFORMATEX*        g_pWfx           = NULL; // negotiated format
static UINT32               g_bufferFrames   = 0;    // WASAPI buffer size in frames
static HANDLE               g_hAudioEvent    = NULL; // WASAPI event-driven mode
static wchar_t              g_audioPath[MAX_PATH] = {0};

// Logging — forward decl (defined in main.cpp)
extern void LogToFile(const char* msg, ...);

// ============================================================
//  Internal helpers
// ============================================================
static void Audio_Cleanup_Internal() {
    if (g_pAudioClient) g_pAudioClient->Stop();

    SafeRel(g_pSimpleVol);
    SafeRel(g_pRenderClient);
    SafeRel(g_pAudioClient);
    SafeRel(g_pDevice);
    SafeRel(g_pEnum);
    SafeRel(g_pAudioReader);

    if (g_pWfx) { CoTaskMemFree(g_pWfx); g_pWfx = NULL; }
    if (g_hAudioEvent) { CloseHandle(g_hAudioEvent); g_hAudioEvent = NULL; }

    g_bufferFrames = 0;
}

// Mở MF audio stream từ file
static bool Audio_OpenReader(const wchar_t* path) {
    HRESULT hr = MFCreateSourceReaderFromURL(path, NULL, &g_pAudioReader);
    if (FAILED(hr)) {
        LogToFile("[Audio] MFCreateSourceReaderFromURL failed: 0x%08X", (unsigned)hr);
        return false;
    }

    // Disable tất cả stream, chỉ enable audio stream đầu tiên
    g_pAudioReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    g_pAudioReader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    // Set output format: PCM float 32-bit — WASAPI native, không cần resample
    IMFMediaType* pType = NULL;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    hr = g_pAudioReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pType);
    SafeRel(pType);

    if (FAILED(hr)) {
        // Fallback: PCM 16-bit
        MFCreateMediaType(&pType);
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        hr = g_pAudioReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pType);
        SafeRel(pType);
        if (FAILED(hr)) {
            LogToFile("[Audio] Cannot set audio output format: 0x%08X", (unsigned)hr);
            SafeRel(g_pAudioReader);
            return false;
        }
        LogToFile("[Audio] Format: PCM 16-bit (fallback)");
    } else {
        LogToFile("[Audio] Format: Float 32-bit");
    }

    // Lấy negotiated format để config WASAPI
    IMFMediaType* pCurrent = NULL;
    hr = g_pAudioReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pCurrent);
    if (FAILED(hr)) {
        LogToFile("[Audio] GetCurrentMediaType failed: 0x%08X", (unsigned)hr);
        SafeRel(g_pAudioReader);
        return false;
    }

    // Convert IMFMediaType → WAVEFORMATEX
    UINT32 wfxSize = 0;
    hr = MFCreateWaveFormatExFromMFMediaType(pCurrent, &g_pWfx, &wfxSize);
    SafeRel(pCurrent);
    if (FAILED(hr)) {
        LogToFile("[Audio] MFCreateWaveFormatExFromMFMediaType failed: 0x%08X", (unsigned)hr);
        SafeRel(g_pAudioReader);
        return false;
    }

    LogToFile("[Audio] Stream: %dHz %dch %dbit",
              g_pWfx->nSamplesPerSec, g_pWfx->nChannels, g_pWfx->wBitsPerSample);
    return true;
}

// Init WASAPI render client với format từ MF
static bool Audio_InitWASAPI() {
    HRESULT hr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&g_pEnum);
    if (FAILED(hr)) {
        LogToFile("[Audio] CoCreateInstance MMDeviceEnumerator: 0x%08X", (unsigned)hr);
        return false;
    }

    hr = g_pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);
    if (FAILED(hr)) {
        LogToFile("[Audio] GetDefaultAudioEndpoint: 0x%08X", (unsigned)hr);
        return false;
    }

    hr = g_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_pAudioClient);
    if (FAILED(hr)) {
        LogToFile("[Audio] Activate IAudioClient: 0x%08X", (unsigned)hr);
        return false;
    }

    // Event-driven mode — AudioClient signal khi cần data
    g_hAudioEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hAudioEvent) {
        LogToFile("[Audio] CreateEvent failed");
        return false;
    }

    // Buffer duration: 200ms — đủ lớn tránh glitch, không quá lớn gây latency
    REFERENCE_TIME bufDur = 2000000LL; // 200ms in 100ns units

    hr = g_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufDur, 0,
        g_pWfx, NULL);

    if (FAILED(hr)) {
        LogToFile("[Audio] IAudioClient::Initialize: 0x%08X", (unsigned)hr);
        // Thử lại với format của device
        WAVEFORMATEX* pDevFmt = NULL;
        g_pAudioClient->GetMixFormat(&pDevFmt);
        if (pDevFmt) {
            hr = g_pAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                bufDur, 0,
                pDevFmt, NULL);
            CoTaskMemFree(pDevFmt);
        }
        if (FAILED(hr)) {
            LogToFile("[Audio] IAudioClient::Initialize retry failed: 0x%08X", (unsigned)hr);
            return false;
        }
    }

    hr = g_pAudioClient->SetEventHandle(g_hAudioEvent);
    if (FAILED(hr)) {
        LogToFile("[Audio] SetEventHandle: 0x%08X", (unsigned)hr);
        return false;
    }

    hr = g_pAudioClient->GetBufferSize(&g_bufferFrames);
    if (FAILED(hr)) {
        LogToFile("[Audio] GetBufferSize: 0x%08X", (unsigned)hr);
        return false;
    }

    hr = g_pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&g_pRenderClient);
    if (FAILED(hr)) {
        LogToFile("[Audio] GetService IAudioRenderClient: 0x%08X", (unsigned)hr);
        return false;
    }

    // Volume control per-session
    hr = g_pAudioClient->GetService(__uuidof(ISimpleAudioVolume), (void**)&g_pSimpleVol);
    if (SUCCEEDED(hr)) {
        g_pSimpleVol->SetMasterVolume(g_audioVolume, NULL);
        g_pSimpleVol->SetMute(g_audioMuted ? TRUE : FALSE, NULL);
    }

    LogToFile("[Audio] WASAPI init OK, buffer=%u frames", g_bufferFrames);
    return true;
}

// ============================================================
//  Audio Thread
// ============================================================
// ============================================================
//  Audio Thread — event-driven, pull-model
//  WASAPI fires g_hAudioEvent khi buffer cần data
//  → tính avail frames → đọc đúng số MF samples cần → push
// ============================================================
static DWORD WINAPI AudioThread(LPVOID) {
    timeBeginPeriod(1);
    LogToFile("[Audio] Thread started");

    // Pre-fill silence 1 buffer để tránh glitch ở đầu
    {
        UINT32 pad = 0;
        g_pAudioClient->GetCurrentPadding(&pad);
        UINT32 avail = g_bufferFrames - pad;
        if (avail > 0) {
            BYTE* pData = NULL;
            if (SUCCEEDED(g_pRenderClient->GetBuffer(avail, &pData)) && pData) {
                ZeroMemory(pData, avail * g_pWfx->nBlockAlign);
                g_pRenderClient->ReleaseBuffer(avail, 0);
            }
        }
    }

    g_pAudioClient->Start();
    LogToFile("[Audio] WASAPI client started");

    const UINT32 blockAlign = g_pWfx->nBlockAlign;

    // Intermediate ring buffer — đủ cho 1 giây audio
    const UINT32 ringMax  = g_pWfx->nSamplesPerSec * blockAlign; // 1 giây
    BYTE*        ringBuf  = (BYTE*)malloc(ringMax);
    UINT32       ringHead = 0; // write pos
    UINT32       ringTail = 0; // read pos
    UINT32       ringUsed = 0; // bytes available to push

    if (!ringBuf) {
        LogToFile("[Audio] OOM ring buffer");
        g_pAudioClient->Stop();
        timeEndPeriod(1);
        return 1;
    }

    // Helper lambdas cho ring buffer
    auto ringWrite = [&](const BYTE* src, UINT32 bytes) -> bool {
        if (bytes > ringMax - ringUsed) return false; // overflow
        UINT32 part1 = min(bytes, ringMax - ringHead);
        memcpy(ringBuf + ringHead, src, part1);
        if (bytes > part1)
            memcpy(ringBuf, src + part1, bytes - part1);
        ringHead = (ringHead + bytes) % ringMax;
        ringUsed += bytes;
        return true;
    };

    auto ringRead = [&](BYTE* dst, UINT32 bytes) -> bool {
        if (bytes > ringUsed) return false;
        UINT32 part1 = min(bytes, ringMax - ringTail);
        memcpy(dst, ringBuf + ringTail, part1);
        if (bytes > part1)
            memcpy(dst + part1, ringBuf, bytes - part1);
        ringTail = (ringTail + bytes) % ringMax;
        ringUsed -= bytes;
        return true;
    };

    bool audioEOS = false;

    while (g_audioRunning) {
        // Handle seek
        if (g_audioSeekPos >= 0) {
            LONGLONG seekTo = g_audioSeekPos;
            g_audioSeekPos  = -1LL;
            ringHead = ringTail = ringUsed = 0; // flush ring
            audioEOS = false;

            PROPVARIANT v; PropVariantInit(&v);
            v.vt = VT_I8; v.hVal.QuadPart = seekTo;
            if (g_pAudioReader)
                g_pAudioReader->SetCurrentPosition(GUID_NULL, v);
            PropVariantClear(&v);
            LogToFile("[Audio] Seeked to %lld", seekTo);
        }

        // Pause handling
        if (g_audioPaused) {
            g_pAudioClient->Stop();
            while (g_audioPaused && g_audioRunning && g_audioSeekPos < 0)
                Sleep(20);
            if (!g_audioRunning) break;
            // Flush WASAPI buffer trước khi resume tránh pop
            {
                UINT32 pad = 0;
                g_pAudioClient->GetCurrentPadding(&pad);
                UINT32 avail = g_bufferFrames - pad;
                if (avail > 0) {
                    BYTE* pData = NULL;
                    if (SUCCEEDED(g_pRenderClient->GetBuffer(avail, &pData)) && pData) {
                        ZeroMemory(pData, avail * blockAlign);
                        g_pRenderClient->ReleaseBuffer(avail, 0);
                    }
                }
            }
            g_pAudioClient->Start();
            continue;
        }

        // === STEP 1: Hỏi WASAPI cần bao nhiêu frames ===
        UINT32 pad   = 0;
        g_pAudioClient->GetCurrentPadding(&pad);
        UINT32 avail = g_bufferFrames - pad; // frames trống trong WASAPI buffer

        if (avail == 0) {
            // Buffer đầy — đợi WASAPI event (max 1 frameDur)
            WaitForSingleObject(g_hAudioEvent, 20);
            continue;
        }

        UINT32 needBytes = avail * blockAlign;

        // === STEP 2: Decode MF samples vào ring buffer cho đến khi đủ data ===
        while (!audioEOS && ringUsed < needBytes && g_audioRunning && g_audioSeekPos < 0) {
            IMFSample* pSamp = NULL;
            DWORD flags = 0, si = 0;
            LONGLONG ts  = 0;

            HRESULT hr = g_pAudioReader->ReadSample(
                MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &si, &flags, &ts, &pSamp);

            if (FAILED(hr)) { audioEOS = true; break; }

            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (pSamp) { pSamp->Release(); pSamp = NULL; }
                audioEOS = true;
                LogToFile("[Audio] EOS");
                break;
            }

            if (!pSamp || (flags & MF_SOURCE_READERF_STREAMTICK)) {
                if (pSamp) { pSamp->Release(); pSamp = NULL; }
                continue;
            }

            IMFMediaBuffer* pBuf = NULL;
            hr = pSamp->ConvertToContiguousBuffer(&pBuf);
            if (SUCCEEDED(hr) && pBuf) {
                BYTE* pData = NULL; DWORD maxLen = 0, curLen = 0;
                hr = pBuf->Lock(&pData, &maxLen, &curLen);
                if (SUCCEEDED(hr) && pData && curLen > 0) {
                    ringWrite(pData, curLen); // overflow được ignore — tự drop
                }
                if (SUCCEEDED(hr)) pBuf->Unlock();
                pBuf->Release();
            }
            pSamp->Release();
        }

        // === STEP 3: Push từ ring buffer vào WASAPI — đúng số avail frames ===
        UINT32 canPushBytes  = min(ringUsed, needBytes);
        UINT32 canPushFrames = canPushBytes / blockAlign;

        if (canPushFrames > 0) {
            BYTE* pRender = NULL;
            HRESULT hr = g_pRenderClient->GetBuffer(canPushFrames, &pRender);
            if (SUCCEEDED(hr) && pRender) {
                // Apply software volume nếu ISimpleAudioVolume không available
                if (!g_pSimpleVol) {
                    float vol = g_audioMuted ? 0.0f : g_audioVolume;
                    // temp copy để scale
                    UINT32 totalBytes = canPushFrames * blockAlign;
                    BYTE* tmp = (BYTE*)alloca(totalBytes);
                    ringRead(tmp, totalBytes);
                    if (g_pWfx->wBitsPerSample == 32) {
                        float* s = (float*)tmp;
                        UINT32 n = totalBytes / 4;
                        for (UINT32 i = 0; i < n; i++) s[i] *= vol;
                    } else if (g_pWfx->wBitsPerSample == 16) {
                        short* s = (short*)tmp;
                        UINT32 n = totalBytes / 2;
                        for (UINT32 i = 0; i < n; i++) s[i] = (short)(s[i] * vol);
                    }
                    memcpy(pRender, tmp, totalBytes);
                } else {
                    // ISimpleAudioVolume lo volume — đọc thẳng từ ring
                    if (g_audioMuted) {
                        ZeroMemory(pRender, canPushFrames * blockAlign);
                        ringTail = (ringTail + canPushFrames * blockAlign) % ringMax;
                        ringUsed -= canPushFrames * blockAlign;
                    } else {
                        ringRead(pRender, canPushFrames * blockAlign);
                    }
                }
                g_pRenderClient->ReleaseBuffer(canPushFrames, 0);
            }
        } else if (audioEOS) {
            // Ring kosong và EOS — đợi seek (loop) hoặc stop
            while (g_audioRunning && g_audioSeekPos < 0 && !g_audioPaused)
                Sleep(10);
        } else {
            // Ring kosong nhưng MF chưa EOS — underrun, push silence
            BYTE* pRender = NULL;
            if (SUCCEEDED(g_pRenderClient->GetBuffer(avail, &pRender)) && pRender) {
                ZeroMemory(pRender, avail * blockAlign);
                g_pRenderClient->ReleaseBuffer(avail, 0);
            }
        }

        // Đợi WASAPI event trước khi lặp
        WaitForSingleObject(g_hAudioEvent, 20);
    }

    free(ringBuf);
    g_pAudioClient->Stop();
    timeEndPeriod(1);
    LogToFile("[Audio] Thread stopped");
    return 0;
}

// ============================================================
//  Public API
// ============================================================
bool AudioSystem_Start(const wchar_t* path, float volume) {
    AudioSystem_Stop(); // cleanup trước

    if (!path || path[0] == L'\0') return false;

    wcscpy_s(g_audioPath, path);
    g_audioVolume  = volume;
    g_audioPaused  = false;
    g_audioMuted   = false;
    g_audioSeekPos = -1LL;

    if (!Audio_OpenReader(path)) {
        LogToFile("[Audio] Failed to open audio reader");
        return false;
    }

    if (!Audio_InitWASAPI()) {
        LogToFile("[Audio] Failed to init WASAPI");
        Audio_Cleanup_Internal();
        return false;
    }

    // Apply initial volume
    if (g_pSimpleVol) {
        g_pSimpleVol->SetMasterVolume(g_audioVolume, NULL);
        g_pSimpleVol->SetMute(FALSE, NULL);
    }

    g_audioRunning = true;
    g_hAudioThread = CreateThread(NULL, 0, AudioThread, NULL, 0, NULL);
    if (!g_hAudioThread) {
        LogToFile("[Audio] CreateThread failed");
        g_audioRunning = false;
        Audio_Cleanup_Internal();
        return false;
    }

    LogToFile("[Audio] System started for: %S", path);
    return true;
}

void AudioSystem_Stop() {
    if (g_hAudioThread) {
        g_audioRunning = false;
        g_audioPaused  = false; // unblock pause loop
        if (g_hAudioEvent) SetEvent(g_hAudioEvent); // unblock wait
        if (WaitForSingleObject(g_hAudioThread, 1000) == WAIT_TIMEOUT) {
            TerminateThread(g_hAudioThread, 0);
            LogToFile("[Audio] Thread forcibly terminated");
        }
        CloseHandle(g_hAudioThread);
        g_hAudioThread = NULL;
    }
    Audio_Cleanup_Internal();
    LogToFile("[Audio] System stopped");
}

void AudioSystem_SetPaused(bool paused) {
    g_audioPaused = paused;
    if (!paused && g_hAudioEvent) SetEvent(g_hAudioEvent); // wake thread
}

void AudioSystem_SetVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    g_audioVolume = volume;
    if (g_pSimpleVol && !g_audioMuted)
        g_pSimpleVol->SetMasterVolume(volume, NULL);
}

void AudioSystem_SetMuted(bool muted) {
    g_audioMuted = muted;
    if (g_pSimpleVol)
        g_pSimpleVol->SetMute(muted ? TRUE : FALSE, NULL);
}

void AudioSystem_Seek(LONGLONG positionHns) {
    g_audioSeekPos = positionHns;
    if (g_hAudioEvent) SetEvent(g_hAudioEvent); // wake thread để xử lý seek ngay
}

bool AudioSystem_IsRunning() {
    return g_audioRunning && g_hAudioThread != NULL;
}
