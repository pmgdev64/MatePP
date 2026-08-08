// audio_system.cpp — FFmpeg + WASAPI audio playback
// Stop là fire-and-forget (detach), Start tạo thread mới ngay lập tức
// Mỗi thread tự quản lý toàn bộ WASAPI + FFmpeg state (không share global)

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include "audio_system.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

extern void LogToFile(const char* msg, ...);

template<typename T>
static inline void SafeRel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ============================================================
//  Shared control (main → thread, atomic)
// ============================================================
static volatile bool  g_audioRunning  = false;
static volatile bool  g_audioPaused   = false;
static volatile bool  g_audioMuted    = false;
static volatile float g_audioVolume   = 1.0f;
static volatile LONG  g_loopPending   = 0;
static volatile LONG  g_generation    = 0;   // tăng mỗi lần Start() — thread cũ tự thoát

// Master clock — vị trí phát hiện tại tính bằng giây, video sync theo cái này.
// Update mỗi lần audio thread ReleaseBuffer thành công (tức là frame đã thực
// sự được đẩy vào hàng đợi phát của thiết bị, gần với "đang nghe" nhất).
static std::atomic<double> g_audioClockSec{0.0};

// True khi g_audioClockSec đã được cập nhật bằng dữ liệu THẬT (đã ReleaseBuffer
// thành công ít nhất 1 lần), phân biệt với "audio thread đã được tạo nhưng còn
// đang init". Reset false mỗi khi Start() hoặc loop-seek, set true lần đầu
// tiên push PCM thật thành công.
static std::atomic<bool> g_audioClockValid{false};

// Thread handle chỉ dùng để IsRunning query, không wait
static HANDLE g_hAudioThread = nullptr;

// Kick event — mỗi thread tạo riêng, pointer share qua StartParams
// Dùng global handle để AudioSystem_NotifyVideoLoop / SetPaused kick được
static HANDLE g_hKickEvent = nullptr;

// Path copy
static wchar_t g_audioPath[MAX_PATH * 4] = {};

// ============================================================
//  Per-thread params (heap, freed bởi thread trước khi thoát)
// ============================================================
struct AudioStartParams {
    wchar_t  path[MAX_PATH * 4];
    float    volume;
    LONG     gen;       // generation lúc thread được tạo
    HANDLE   kickEvent; // auto-reset event — thread tự CloseHandle khi thoát
};

// ============================================================
//  Ring buffer
// ============================================================
struct RingBuf {
    BYTE*  data     = nullptr;
    UINT32 capacity = 0;
    UINT32 head     = 0;
    UINT32 tail     = 0;
    UINT32 used     = 0;

    bool alloc(UINT32 bytes) { data = (BYTE*)malloc(bytes); if (!data) return false; capacity = bytes; return true; }
    void free_() { free(data); data = nullptr; capacity = used = head = tail = 0; }
    void flush() { head = tail = used = 0; }

    bool write(const BYTE* src, UINT32 bytes) {
        if (bytes > capacity - used) return false;
        UINT32 p1 = std::min(bytes, capacity - head);
        memcpy(data + head, src, p1);
        if (bytes > p1) memcpy(data, src + p1, bytes - p1);
        head = (head + bytes) % capacity;
        used += bytes;
        return true;
    }
    bool read(BYTE* dst, UINT32 bytes) {
        if (bytes > used) return false;
        UINT32 p1 = std::min(bytes, capacity - tail);
        memcpy(dst, data + tail, p1);
        if (bytes > p1) memcpy(dst + p1, data + tail + p1 - capacity, bytes - p1);
        tail = (tail + bytes) % capacity;
        used -= bytes;
        return true;
    }
    void discard(UINT32 bytes) { bytes = std::min(bytes, used); tail = (tail + bytes) % capacity; used -= bytes; }
};

// ============================================================
//  Audio Thread — toàn bộ WASAPI + FFmpeg state là local
// ============================================================
static DWORD WINAPI AudioThread(LPVOID pArg) {
    AudioStartParams* p = (AudioStartParams*)pArg;
    LONG myGen    = p->gen;
    HANDLE myKick = p->kickEvent;
    char path_utf8[MAX_PATH * 4] = {};
    WideCharToMultiByte(CP_UTF8, 0, p->path, -1, path_utf8, sizeof(path_utf8) - 1, nullptr, nullptr);
    delete p;

    timeBeginPeriod(1);
    LogToFile("[Audio] Thread gen=%d started", (int)myGen);

    // ---- local WASAPI state ----
    IMMDeviceEnumerator* pEnum        = nullptr;
    IMMDevice*           pDevice      = nullptr;
    IAudioClient*        pAudioClient = nullptr;
    IAudioRenderClient*  pRender      = nullptr;
    ISimpleAudioVolume*  pSimpleVol   = nullptr;
    WAVEFORMATEX*        pWfx         = nullptr;
    UINT32               bufFrames    = 0;

    // ---- local FFmpeg state ----
    AVFormatContext* pFmt    = nullptr;
    AVCodecContext*  pCodec  = nullptr;
    AVFrame*         pFrame  = av_frame_alloc();
    AVPacket*        pPacket = av_packet_alloc();
    SwrContext*      pSwr    = nullptr;
    int              audioIdx = -1;

    auto isStale = [&]() -> bool {
        return g_generation != myGen || !g_audioRunning;
    };

    auto cleanup = [&]() {
        if (pSwr)    swr_free(&pSwr);
        if (pCodec)  avcodec_free_context(&pCodec);
        if (pFmt)    avformat_close_input(&pFmt);
        if (pFrame)  av_frame_free(&pFrame);
        if (pPacket) av_packet_free(&pPacket);
        if (pAudioClient) pAudioClient->Stop();
        SafeRel(pSimpleVol);
        SafeRel(pRender);
        SafeRel(pAudioClient);
        SafeRel(pDevice);
        SafeRel(pEnum);
        if (pWfx) { CoTaskMemFree(pWfx); pWfx = nullptr; }
        CloseHandle(myKick);
        timeEndPeriod(1);
        // Nếu thread này vẫn là generation hiện hành (tức là tự thoát do lỗi/EOF,
        // không phải bị Start()/Stop() khác bump gen) thì phải hạ g_audioRunning
        // xuống false, nếu không AudioSystem_IsRunning()/GetClockSec() sẽ báo sai
        // là audio vẫn đang chạy dù thread đã chết (vd. file không có audio stream).
        if (g_generation == myGen) g_audioRunning = false;
        LogToFile("[Audio] Thread gen=%d stopped", (int)myGen);
    };

    if (!pFrame || !pPacket) { cleanup(); return 0; }

    // ---- FFmpeg open ----
    // Audio chỉ cần tìm 1 stream audio, không cần probe sâu như video decoder
    // (video decoder cần full probe để biết codec/PTS chính xác cho timing).
    // Giảm probesize/analyzeduration giúp avformat_open_input + find_stream_info
    // nhanh hơn đáng kể trên HDD, đặc biệt vì audio mở file LẦN THỨ HAI
    // (video decoder đã probe lần đầu) — không cần probe kỹ lại từ đầu.
    pFmt = avformat_alloc_context();
    if (pFmt) {
        pFmt->probesize = 512 * 1024;        // 512KB thay vì mặc định ~5MB
        pFmt->max_analyze_duration = 1000000; // 1 giây (đơn vị AV_TIME_BASE = micro-sec) thay vì mặc định 5s
    }
    if (avformat_open_input(&pFmt, path_utf8, nullptr, nullptr) < 0) {
        LogToFile("[Audio] avformat_open_input failed");
        cleanup(); return 0;
    }
    avformat_find_stream_info(pFmt, nullptr);

    for (unsigned i = 0; i < pFmt->nb_streams; i++) {
        if (pFmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIdx = (int)i; break;
        }
    }
    if (audioIdx < 0) { LogToFile("[Audio] No audio stream"); cleanup(); return 0; }
    if (isStale())    { cleanup(); return 0; }

    {
        AVCodecParameters* cp    = pFmt->streams[audioIdx]->codecpar;
        const AVCodec*     codec = avcodec_find_decoder(cp->codec_id);
        if (!codec) { LogToFile("[Audio] No decoder"); cleanup(); return 0; }
        pCodec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(pCodec, cp);
        if (avcodec_open2(pCodec, codec, nullptr) < 0) { LogToFile("[Audio] avcodec_open2 failed"); cleanup(); return 0; }

        // WASAPI init
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&pEnum))
            || FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))
            || FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient))
            || FAILED(pAudioClient->GetMixFormat(&pWfx))) {
            LogToFile("[Audio] WASAPI init failed"); cleanup(); return 0;
        }
        REFERENCE_TIME bufDur = 2000000LL;
        if (FAILED(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             bufDur, 0, pWfx, nullptr))
            || FAILED(pAudioClient->SetEventHandle(myKick))
            || FAILED(pAudioClient->GetBufferSize(&bufFrames))
            || FAILED(pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRender))) {
            LogToFile("[Audio] WASAPI configure failed"); cleanup(); return 0;
        }
        if (SUCCEEDED(pAudioClient->GetService(__uuidof(ISimpleAudioVolume), (void**)&pSimpleVol))) {
            pSimpleVol->SetMasterVolume(g_audioVolume, nullptr);
            pSimpleVol->SetMute(g_audioMuted ? TRUE : FALSE, nullptr);
        }
        LogToFile("[Audio] WASAPI OK %dHz %dch", pWfx->nSamplesPerSec, pWfx->nChannels);

        // SWR
        AVSampleFormat swrOutFmt = (pWfx->wBitsPerSample == 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLT;
        pSwr = swr_alloc();
        AVChannelLayout outLayout;
        if (pWfx->nChannels == 1) { AVChannelLayout tmp = AV_CHANNEL_LAYOUT_MONO;   outLayout = tmp; }
        else                      { AVChannelLayout tmp = AV_CHANNEL_LAYOUT_STEREO; outLayout = tmp; }
        av_opt_set_chlayout  (pSwr, "in_chlayout",    &pCodec->ch_layout,     0);
        av_opt_set_int       (pSwr, "in_sample_rate",  pCodec->sample_rate,    0);
        av_opt_set_sample_fmt(pSwr, "in_sample_fmt",   pCodec->sample_fmt,     0);
        av_opt_set_chlayout  (pSwr, "out_chlayout",   &outLayout,              0);
        av_opt_set_int       (pSwr, "out_sample_rate", pWfx->nSamplesPerSec,   0);
        av_opt_set_sample_fmt(pSwr, "out_sample_fmt",  swrOutFmt,              0);
        if (swr_init(pSwr) < 0) { LogToFile("[Audio] swr_init failed"); cleanup(); return 0; }
    }

    if (isStale()) { cleanup(); return 0; }

    // ---- Ring buffer + playback loop ----
    const UINT32 blockAlign = pWfx->nBlockAlign;
    RingBuf ring;
    if (!ring.alloc(pWfx->nSamplesPerSec * blockAlign)) { cleanup(); return 0; }

    const int swrOutMax = 2048;
    BYTE* swrTmp = (BYTE*)malloc((size_t)swrOutMax * blockAlign);
    if (!swrTmp) { ring.free_(); cleanup(); return 0; }

    // Pre-fill silence
    { UINT32 pad=0; pAudioClient->GetCurrentPadding(&pad); UINT32 avail=bufFrames-pad;
      BYTE* pd=nullptr;
      if (avail>0 && SUCCEEDED(pRender->GetBuffer(avail,&pd)) && pd)
        { ZeroMemory(pd,avail*blockAlign); pRender->ReleaseBuffer(avail,0); } }
    pAudioClient->Start();

    bool eos = false;
    UINT64 framesPushedTotal = 0;   // tổng số PCM frame đã đẩy cho WASAPI từ đầu file
    g_audioClockSec.store(0.0);     // reset master clock cho lần phát mới

    while (!isStale()) {
        // Loop signal
        if (InterlockedExchange(&g_loopPending, 0)) {
            ring.flush(); eos = false;
            av_seek_frame(pFmt, audioIdx, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(pCodec);
            swr_init(pSwr);
            framesPushedTotal = 0;
            g_audioClockSec.store(0.0);
            g_audioClockValid.store(false);   // video se fallback ve wall-clock cho
                                               // toi khi audio that su co PCM moi sau
                                               // seek, tranh khung lap moi lan loop
            LogToFile("[Audio] Loop seeked");
        }

        // Pause
        if (g_audioPaused) {
            pAudioClient->Stop();
            while (g_audioPaused && !isStale() && !g_loopPending)
                Sleep(20);
            if (isStale()) break;
            { UINT32 pad=0,avail=0; pAudioClient->GetCurrentPadding(&pad); avail=bufFrames-pad;
              BYTE* pd=nullptr;
              if (avail>0 && SUCCEEDED(pRender->GetBuffer(avail,&pd)) && pd)
                { ZeroMemory(pd,avail*blockAlign); pRender->ReleaseBuffer(avail,0); } }
            pAudioClient->Start();
            continue;
        }

        UINT32 pad=0; pAudioClient->GetCurrentPadding(&pad);
        UINT32 avail = bufFrames - pad;
        if (avail == 0) { WaitForSingleObject(myKick, 20); continue; }
        UINT32 needBytes = avail * blockAlign;

        // Decode → ring
        while (!eos && ring.used < needBytes && !isStale() && !g_loopPending) {
            int ret = av_read_frame(pFmt, pPacket);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    avcodec_send_packet(pCodec, nullptr);
                    while (avcodec_receive_frame(pCodec, pFrame) == 0) {
                        uint8_t* out[1]={swrTmp};
                        int n = swr_convert(pSwr,out,swrOutMax,(const uint8_t**)pFrame->data,pFrame->nb_samples);
                        if (n>0) ring.write(swrTmp,(UINT32)n*blockAlign);
                        av_frame_unref(pFrame);
                    }
                    eos = true;
                }
                av_packet_unref(pPacket); break;
            }
            if (pPacket->stream_index != audioIdx) { av_packet_unref(pPacket); continue; }
            if (avcodec_send_packet(pCodec, pPacket) >= 0) {
                while (avcodec_receive_frame(pCodec, pFrame) == 0) {
                    uint8_t* out[1]={swrTmp};
                    int n = swr_convert(pSwr,out,swrOutMax,(const uint8_t**)pFrame->data,pFrame->nb_samples);
                    if (n>0) ring.write(swrTmp,(UINT32)n*blockAlign);
                    av_frame_unref(pFrame);
                }
            }
            av_packet_unref(pPacket);
        }

        // Push → WASAPI
        UINT32 canBytes  = std::min(ring.used, needBytes);
        UINT32 canFrames = canBytes / blockAlign;

        if (canFrames > 0) {
            BYTE* pd=nullptr;
            if (SUCCEEDED(pRender->GetBuffer(canFrames, &pd)) && pd) {
                if (g_audioMuted) { ZeroMemory(pd,canFrames*blockAlign); ring.discard(canFrames*blockAlign); }
                else {
                    ring.read(pd, canFrames*blockAlign);
                    if (!pSimpleVol) {
                        float vol = g_audioVolume;
                        if (pWfx->wBitsPerSample==32) { float* s=(float*)pd; for(UINT32 i=0;i<canFrames*pWfx->nChannels;i++) s[i]*=vol; }
                        else if(pWfx->wBitsPerSample==16) { short* s=(short*)pd; for(UINT32 i=0;i<canFrames*pWfx->nChannels;i++) s[i]=(short)(s[i]*vol); }
                    }
                }
                pRender->ReleaseBuffer(canFrames, 0);

                // Cập nhật master clock: framesPushedTotal là tổng đã đẩy vào WASAPI,
                // trừ đi phần còn đang nằm trong buffer phần cứng (pad) sẽ ra vị trí
                // PCM thực sự đang phát ra loa lúc này — chính xác hơn là đếm theo
                // thời điểm ReleaseBuffer (lúc đó audio chưa thực sự phát ra).
                framesPushedTotal += canFrames;
                UINT32 padNow = 0;
                pAudioClient->GetCurrentPadding(&padNow);
                UINT64 playedFrames = (framesPushedTotal > padNow) ? (framesPushedTotal - padNow) : 0;
                g_audioClockSec.store((double)playedFrames / (double)pWfx->nSamplesPerSec);
                g_audioClockValid.store(true);   // tu day audioSec moi dang tin
            }
        } else if (eos) {
            while (!isStale() && !g_loopPending && !g_audioPaused) Sleep(10);
        } else {
            BYTE* pd=nullptr;
            if (avail>0 && SUCCEEDED(pRender->GetBuffer(avail,&pd)) && pd)
                { ZeroMemory(pd,avail*blockAlign); pRender->ReleaseBuffer(avail,0); }
        }

        WaitForSingleObject(myKick, 20);
    }

    free(swrTmp);
    ring.free_();
    cleanup();
    return 0;
}

// ============================================================
//  Public API
// ============================================================
bool AudioSystem_Start(const wchar_t* path, float volume) {
    // Signal thread cũ thoát (nếu có) — không wait
    g_audioRunning = false;
    g_audioPaused  = false;
    if (g_hKickEvent) SetEvent(g_hKickEvent);
    if (g_hAudioThread) { CloseHandle(g_hAudioThread); g_hAudioThread = nullptr; }

    if (!path || path[0] == L'\0') return false;

    // Bump generation — thread cũ sẽ thấy isStale() == true và tự thoát
    LONG gen = InterlockedIncrement(&g_generation);

    g_audioVolume  = (volume < 0.f ? 0.f : volume > 1.f ? 1.f : volume);
    g_audioPaused  = false;
    g_audioMuted   = false;
    g_loopPending  = 0;
    g_audioClockSec.store(0.0);
    g_audioClockValid.store(false);   // audio thread vua duoc tao, chua co PCM that
    g_audioRunning = true;

    // Tạo kick event mới cho thread mới
    HANDLE kick = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_hKickEvent = kick;  // store để NotifyVideoLoop / SetPaused dùng

    AudioStartParams* p = new AudioStartParams();
    wcscpy_s(p->path, path);
    p->volume    = volume;
    p->gen       = gen;
    p->kickEvent = kick;

    g_hAudioThread = CreateThread(nullptr, 0, AudioThread, p, 0, nullptr);
    if (!g_hAudioThread) {
        LogToFile("[Audio] CreateThread failed: %d", GetLastError());
        g_audioRunning = false;
        CloseHandle(kick);
        g_hKickEvent = nullptr;
        delete p;
        return false;
    }

    LogToFile("[Audio] Started gen=%d: %S", (int)gen, path);
    return true;
}

void AudioSystem_Stop() {
    if (!g_hAudioThread) return;
    g_audioRunning = false;
    g_audioPaused  = false;
    InterlockedIncrement(&g_generation);  // invalidate thread
    if (g_hKickEvent) SetEvent(g_hKickEvent);
    // Detach — thread tự dọn và thoát, không block caller
    CloseHandle(g_hAudioThread);
    g_hAudioThread = nullptr;
    LogToFile("[Audio] Stop posted (detached)");
}

void AudioSystem_SetPaused(bool paused) {
    g_audioPaused = paused;
    if (!paused && g_hKickEvent) SetEvent(g_hKickEvent);
}

void AudioSystem_SetVolume(float volume) {
    if (volume < 0.f) volume = 0.f;
    if (volume > 1.f) volume = 1.f;
    g_audioVolume = volume;
    // ISimpleAudioVolume là local thread — không thể set từ đây
    // Thread sẽ đọc g_audioVolume qua software path nếu pSimpleVol unavailable
    // Để update ngay: gửi kick ép thread process lại volume
    if (g_hKickEvent) SetEvent(g_hKickEvent);
}

void AudioSystem_SetMuted(bool muted) {
    g_audioMuted = muted;
    if (g_hKickEvent) SetEvent(g_hKickEvent);
}

void AudioSystem_NotifyVideoLoop() {
    InterlockedExchange(&g_loopPending, 1);
    if (g_hKickEvent) SetEvent(g_hKickEvent);
}

bool AudioSystem_IsRunning() {
    return g_audioRunning && g_hAudioThread != nullptr;
}

bool AudioSystem_HasValidClock() {
    return g_audioRunning && g_audioClockValid.load();
}

double AudioSystem_GetClockSec() {
    if (!g_audioRunning) return 0.0;
    return g_audioClockSec.load();
}
