// video_decoder.cpp - KHÔNG CÒN AUDIO
#include <windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <cstdarg>
#include <algorithm>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
// KHÔNG CẦN libswresample
}

extern ID2D1HwndRenderTarget* g_pRT;
extern ID2D1Bitmap*     g_pVideoBmp;
extern UINT  g_vidW, g_vidH;
extern DWORD g_frameDur, g_lastTick;
extern bool  g_vidLoaded, g_useVideo;
extern std::mutex g_vidMtx;

extern BYTE* g_pixBuf[2];
extern int   g_pixBack;
extern std::mutex   g_pixMtx;
extern volatile bool g_newFrameReady;
extern HANDLE g_hFrameConsumed;

extern bool g_isPaused;
extern bool g_isLooping;
extern volatile LONG g_exceptionCount;

void LogToFile(const char* msg, ...);
// KHÔNG CÒN AUDIO FUNCTIONS
// void AudioSystem_NotifyVideoLoop();
// double AudioSystem_GetClockSec();
// bool   AudioSystem_IsRunning();
// bool   AudioSystem_HasValidClock();

static volatile LONG g_decodeGen     = 0;
static HANDLE        g_hDecodeThread = NULL;
static std::mutex g_pixLifeMtx;

static LARGE_INTEGER g_qpcFreq = {0};
static LONGLONG GetNow100ns() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return now.QuadPart * 10000000LL / g_qpcFreq.QuadPart;
}

static enum AVPixelFormat GetD3D11Format(AVCodecContext* /*ctx*/, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    return pix_fmts[0];
}

static bool CodecSupportsD3D11VA(const AVCodec* codec) {
    for (int i = 0;; i++) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        if (cfg->pix_fmt == AV_PIX_FMT_D3D11 &&
            (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return true;
        }
    }
    return false;
}

struct DecodeParams {
    AVFormatContext* pFmtCtx;
    int  vidIdx;
    UINT vidW, vidH;
    int  srcW, srcH;
    LONG gen;
};

static DWORD WINAPI DecodeThreadProc(LPVOID pArg) {
    LONGLONG tThreadStart = GetNow100ns();
    DecodeParams* dp = (DecodeParams*)pArg;
    LONG myGen = dp->gen;

    AVFormatContext* pFmtCtx = dp->pFmtCtx;
    int  vidIdx = dp->vidIdx;
    UINT vidW = dp->vidW, vidH = dp->vidH;
    int  srcW = dp->srcW, srcH = dp->srcH;
    delete dp;

    AVCodecContext*  pCodecCtx  = NULL;
    AVFrame*         pFrame     = NULL;
    AVPacket*        pPacket    = NULL;
    SwsContext*      pSwsCtx    = NULL;
    AVBufferRef*     hwDeviceCtx = NULL;
    bool             usingHwDecode = false;
    enum AVPixelFormat swsSrcFmt = AV_PIX_FMT_NONE;

    // 2 buffers BGRA
    BYTE* myBuf[2] = {NULL, NULL};
    int   myBack   = 0;
    UINT  bufSize = vidW * vidH * 4;

    auto isStale = [&]() -> bool { return g_decodeGen != myGen; };

    auto cleanup = [&]() {
        {
            std::lock_guard<std::mutex> lk(g_pixLifeMtx);
            if (!isStale()) {
                g_newFrameReady = false;
                g_pixBuf[0] = NULL;
                g_pixBuf[1] = NULL;
            }
        }
        free(myBuf[0]); myBuf[0] = NULL;
        free(myBuf[1]); myBuf[1] = NULL;

        if (pPacket)    av_packet_free(&pPacket);
        if (pFrame)     av_frame_free(&pFrame);
        if (pSwsCtx)    sws_freeContext(pSwsCtx);
        if (pCodecCtx)  avcodec_free_context(&pCodecCtx);
        if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
        if (pFmtCtx)    avformat_close_input(&pFmtCtx);
        LogToFile("[Decode] gen=%d cleaned up", (int)myGen);
    };

    myBuf[0] = (BYTE*)calloc(1, bufSize);
    myBuf[1] = (BYTE*)calloc(1, bufSize);
    if (!myBuf[0] || !myBuf[1]) {
        LogToFile("[Decode] OOM pixbuf");
        cleanup();
        return 1;
    }

    {
        std::lock_guard<std::mutex> lk(g_pixLifeMtx);
        g_pixBuf[0] = myBuf[0];
        g_pixBuf[1] = myBuf[1];
        g_pixBack   = 0;
        myBack      = 0;
    }

    if (!pFmtCtx || vidIdx < 0) { LogToFile("[Decode] Invalid params"); cleanup(); return 1; }

    AVCodecParameters* cp    = pFmtCtx->streams[vidIdx]->codecpar;
    const AVCodec*     codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) { LogToFile("[Decode] No decoder"); cleanup(); return 1; }

    bool tryHw = CodecSupportsD3D11VA(codec);
    if (tryHw) {
        int hwErr = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0);
        if (hwErr < 0) {
            char errbuf[128]; av_strerror(hwErr, errbuf, sizeof(errbuf));
            LogToFile("[Decode] D3D11VA failed (%s), fallback CPU", errbuf);
            hwDeviceCtx = NULL;
            tryHw = false;
        } else {
            LogToFile("[Decode] D3D11VA enabled");
        }
    }

    pCodecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(pCodecCtx, cp);

    if (tryHw) {
        pCodecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
        pCodecCtx->get_format    = GetD3D11Format;
        pCodecCtx->thread_count  = 1;
        usingHwDecode = true;
    } else {
        pCodecCtx->thread_count = 2;
    }

    if (avcodec_open2(pCodecCtx, codec, NULL) < 0) {
        if (usingHwDecode) {
            LogToFile("[Decode] hw open failed, retry software");
            avcodec_free_context(&pCodecCtx);
            if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
            usingHwDecode = false;
            pCodecCtx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(pCodecCtx, cp);
            pCodecCtx->thread_count = 2;
            if (avcodec_open2(pCodecCtx, codec, NULL) < 0) {
                LogToFile("[Decode] sw open failed"); cleanup(); return 1;
            }
        } else {
            LogToFile("[Decode] avcodec_open2 failed"); cleanup(); return 1;
        }
    }

    pFrame  = av_frame_alloc();
    pPacket = av_packet_alloc();
    if (!pFrame || !pPacket) { cleanup(); return 1; }

    LogToFile("[Decode] gen=%d started %dx%d (BGRA, no audio)", (int)myGen, vidW, vidH);
    timeBeginPeriod(1);

    // KHÔNG CÒN AUDIO SYNC - dùng wall-clock đơn giản
    bool timingInit = false;
    LONGLONG startPTS = 0, startWall = 0;

    DWORD lastLog = timeGetTime();
    LONG  frameCount = 0;
    bool  firstFrameLogged = false;
    bool  loopPending = false;
    LONGLONG tLoopStart = 0;

    while (!isStale()) {
        if (g_isPaused || !g_vidLoaded || !g_pRT) {
            Sleep(16);
            timingInit = false;
            continue;
        }
        if (g_newFrameReady) { Sleep(1); continue; }

        int ret = av_read_frame(pFmtCtx, pPacket);
        if (ret < 0) {
            if (ret == AVERROR_EOF && g_isLooping && !isStale()) {
                av_seek_frame(pFmtCtx, vidIdx, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(pCodecCtx);
                tLoopStart = GetNow100ns();
                loopPending = true;
                timingInit = false;
                // KHÔNG CÒN AudioSystem_NotifyVideoLoop();
            }
            av_packet_unref(pPacket);
            Sleep(5);
            continue;
        }

        if (pPacket->stream_index != vidIdx) { av_packet_unref(pPacket); continue; }

        if (avcodec_send_packet(pCodecCtx, pPacket) < 0) { av_packet_unref(pPacket); continue; }
        av_packet_unref(pPacket);

        if (avcodec_receive_frame(pCodecCtx, pFrame) != 0) continue;
        if (!pFrame->data[0]) { av_frame_unref(pFrame); continue; }
        if (isStale()) { av_frame_unref(pFrame); break; }

        AVFrame* swFrame = NULL;
        AVFrame* srcFrame = pFrame;
        if (pFrame->format == AV_PIX_FMT_D3D11) {
            swFrame = av_frame_alloc();
            if (swFrame && av_hwframe_transfer_data(swFrame, pFrame, 0) < 0) {
                if (swFrame) av_frame_free(&swFrame);
                av_frame_unref(pFrame);
                continue;
            }
            srcFrame = swFrame;
        }

        // WALL-CLOCK SYNC - ĐƠN GIẢN, KHÔNG CẦN AUDIO
        if (pFrame->pts != AV_NOPTS_VALUE) {
            AVStream* s = pFmtCtx->streams[vidIdx];
            if (s && s->time_base.num > 0) {
                LONGLONG pts = pFrame->pts * s->time_base.num * 10000000LL / s->time_base.den;
                if (!timingInit) {
                    startPTS = pts;
                    startWall = GetNow100ns();
                    timingInit = true;
                }
                LONGLONG w = (pts - startPTS) - (GetNow100ns() - startWall);
                if (w > 20000LL && w < 50000000LL) {
                    Sleep((DWORD)(w / 10000LL));
                } else if (w < -10000000LL) {
                    timingInit = false;
                }
            }
        }

        if (!pSwsCtx || swsSrcFmt != (AVPixelFormat)srcFrame->format) {
            if (pSwsCtx) { sws_freeContext(pSwsCtx); pSwsCtx = NULL; }
            swsSrcFmt = (AVPixelFormat)srcFrame->format;
            pSwsCtx = sws_getContext(srcW, srcH, swsSrcFmt,
                                      vidW, vidH, AV_PIX_FMT_BGRA,
                                      SWS_FAST_BILINEAR, NULL, NULL, NULL);
            if (!pSwsCtx) {
                if (swFrame) av_frame_free(&swFrame);
                av_frame_unref(pFrame);
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_pixLifeMtx);
            if (isStale() || !myBuf[0] || !myBuf[1] || !g_pRT) {
                if (swFrame) av_frame_free(&swFrame);
                av_frame_unref(pFrame);
                break;
            }

            BYTE* dst = myBuf[myBack];
            uint8_t* d[1] = { dst };
            int ds[1] = { (int)vidW * 4 };

            sws_scale(pSwsCtx, srcFrame->data, srcFrame->linesize, 0, srcFrame->height, d, ds);

            if (swFrame) av_frame_free(&swFrame);
            av_frame_unref(pFrame);
            myBack = 1 - myBack;
            g_pixBack = myBack;
            g_newFrameReady = true;
        }
        frameCount++;

        if (!firstFrameLogged) {
            firstFrameLogged = true;
            LogToFile("[Decode] FIRST frame: %lldms", (GetNow100ns() - tThreadStart) / 10000);
        }
        if (loopPending) {
            loopPending = false;
            LogToFile("[Decode] AFTER-LOOP: %lldms", (GetNow100ns() - tLoopStart) / 10000);
        }

        DWORD now = timeGetTime();
        if (now - lastLog > 5000) {
            LogToFile("[Decode] gen=%d %d frames", (int)myGen, frameCount);
            lastLog = now; frameCount = 0;
        }
    }

    timeEndPeriod(1);
    LogToFile("[Decode] gen=%d exited", (int)myGen);
    cleanup();
    return 0;
}

// ============================================================
//  Public API
// ============================================================
bool LoadVideo(const wchar_t* path, ID2D1RenderTarget* rt) {
    if (!path || !rt) return false;
    std::lock_guard<std::mutex> lk(g_vidMtx);

    InterlockedIncrement(&g_decodeGen);
    g_vidLoaded     = false;
    g_newFrameReady = false;
    if (g_hFrameConsumed) SetEvent(g_hFrameConsumed);

    if (g_hDecodeThread) { CloseHandle(g_hDecodeThread); g_hDecodeThread = NULL; }

    {
        std::lock_guard<std::mutex> lk2(g_pixLifeMtx);
        g_pixBuf[0] = NULL;
        g_pixBuf[1] = NULL;
    }

    if (g_pVideoBmp) { g_pVideoBmp->Release(); g_pVideoBmp = NULL; }

    char path_utf8[MAX_PATH * 4] = {0};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path_utf8, sizeof(path_utf8) - 1, NULL, NULL);
    LogToFile("[Load] Opening: %s", path_utf8);

    AVFormatContext* pOpenedFmt = avformat_alloc_context();
    if (pOpenedFmt) {
        pOpenedFmt->probesize = 512 * 1024;
        pOpenedFmt->max_analyze_duration = 1000000;
    }
    if (avformat_open_input(&pOpenedFmt, path_utf8, NULL, NULL) < 0) {
        LogToFile("[ERR] avformat_open_input failed"); return false;
    }
    avformat_find_stream_info(pOpenedFmt, NULL);

    int vidIdx = -1;
    for (unsigned i = 0; i < pOpenedFmt->nb_streams; i++) {
        if (pOpenedFmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vidIdx = i; break; }
    }
    if (vidIdx < 0) { LogToFile("[ERR] No video stream"); avformat_close_input(&pOpenedFmt); return false; }

    AVCodecParameters* cp = pOpenedFmt->streams[vidIdx]->codecpar;
    if (!avcodec_find_decoder(cp->codec_id)) {
        LogToFile("[ERR] No decoder"); avformat_close_input(&pOpenedFmt); return false;
    }

    int srcW = cp->width, srcH = cp->height;
    g_vidW = (UINT)srcW;
    g_vidH = (UINT)srcH;

    AVRational fr = pOpenedFmt->streams[vidIdx]->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = pOpenedFmt->streams[vidIdx]->r_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) { fr.num = 30; fr.den = 1; }
    g_frameDur = std::max(1, (int)(1000.0 * fr.den / fr.num));

    D2D1_SIZE_U sz = D2D1::SizeU(g_vidW, g_vidH);
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(rt->CreateBitmap(sz, NULL, 0, bp, &g_pVideoBmp))) {
        LogToFile("[ERR] CreateBitmap failed"); return false;
    }

    if (!g_hFrameConsumed) g_hFrameConsumed = CreateEvent(NULL, FALSE, TRUE, NULL);
    QueryPerformanceFrequency(&g_qpcFreq);

    DecodeParams* dp = new DecodeParams();
    dp->pFmtCtx = pOpenedFmt;
    dp->vidIdx  = vidIdx;
    dp->srcW = srcW; dp->srcH = srcH;
    dp->vidW = g_vidW; dp->vidH = g_vidH;
    dp->gen  = g_decodeGen;

    g_vidLoaded = true;
    g_hDecodeThread = CreateThread(NULL, 0, DecodeThreadProc, dp, 0, NULL);
    if (!g_hDecodeThread) {
        LogToFile("[ERR] CreateThread failed");
        g_vidLoaded = false;
        if (g_pVideoBmp) { g_pVideoBmp->Release(); g_pVideoBmp = NULL; }
        avformat_close_input(&pOpenedFmt);
        delete dp;
        return false;
    }

    LogToFile("[Load] OK %dx%d @%dfps gen=%d (NO AUDIO)", g_vidW, g_vidH, 1000/g_frameDur, (int)g_decodeGen);
    return true;
}

void StopDecodeThread() {
    InterlockedIncrement(&g_decodeGen);
    g_vidLoaded     = false;
    g_newFrameReady = false;
    if (g_hFrameConsumed) SetEvent(g_hFrameConsumed);
    if (g_hDecodeThread) { CloseHandle(g_hDecodeThread); g_hDecodeThread = NULL; }
    {
        std::lock_guard<std::mutex> lk(g_pixLifeMtx);
        g_pixBuf[0] = NULL;
        g_pixBuf[1] = NULL;
    }
    if (g_pVideoBmp) { g_pVideoBmp->Release(); g_pVideoBmp = NULL; }
}

bool ReadVideoFrame() {
    if (!g_newFrameReady || !g_pVideoBmp) return false;
    int front;
    {
        std::lock_guard<std::mutex> lk(g_pixMtx);
        if (!g_newFrameReady) return false;
        front = 1 - g_pixBack;
        g_newFrameReady = false;
    }

    std::lock_guard<std::mutex> lk2(g_pixLifeMtx);
    BYTE* src = g_pixBuf[front];
    if (!src || !g_pVideoBmp) return false;

    D2D1_RECT_U r = D2D1::RectU(0, 0, g_vidW, g_vidH);
    g_pVideoBmp->CopyFromMemory(&r, src, g_vidW * 4);
    if (g_hFrameConsumed) SetEvent(g_hFrameConsumed);
    return true;
}

D2D1_RECT_F VidLetterbox(float sw, float sh) {
    if (!g_vidW || !g_vidH) return D2D1::RectF(0, 0, sw, sh);
    float s = std::min(sw / g_vidW, sh / g_vidH);
    float dw = g_vidW * s, dh = g_vidH * s;
    return D2D1::RectF((sw-dw)*.5f, (sh-dh)*.5f, (sw+dw)*.5f, (sh+dh)*.5f);
}
