// video_decoder.cpp - WORKING VERSION (at20260629)
#include <windows.h>
#include <d2d1.h>
#include <mutex>
#include <cstdarg>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
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

static AVFormatContext*  g_pFmtCtx     = NULL;
static AVCodecContext*   g_pCodecCtx   = NULL;
static AVFrame*          g_pFrame      = NULL;
static AVPacket*         g_pPacket     = NULL;
static SwsContext*       g_pSwsCtx     = NULL;
static int               g_vidStreamIdx = -1;
static int               g_srcWidth     = 0;
static int               g_srcHeight    = 0;

static volatile LONG g_decodeRunning = 0;
static HANDLE g_hDecodeThread = NULL;
static volatile LONG g_totalFrames = 0;
static LARGE_INTEGER g_qpcFreq = {0};

static LONGLONG GetNow100ns() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 10000000LL / g_qpcFreq.QuadPart;
}

static DWORD WINAPI DecodeThreadProc(LPVOID) {
    if (!g_pFmtCtx || !g_pCodecCtx || !g_pFrame || !g_pPacket || g_vidStreamIdx < 0) {
        LogToFile("[Decode] FATAL: NULL pointer");
        return 1;
    }

    LogToFile("[Decode] STARTED - %dx%d", g_srcWidth, g_srcHeight);

    timeBeginPeriod(1);
    LONG frameCount = 0;
    DWORD lastLog = timeGetTime();
    bool timingInit = false;
    LONGLONG startPTS = 0, startWall = 0;

    while (g_decodeRunning) {
        if (g_isPaused || !g_vidLoaded) {
            Sleep(16);
            timingInit = false;
            continue;
        }

        if (g_newFrameReady) {
            Sleep(1);
            continue;
        }

        int ret = av_read_frame(g_pFmtCtx, g_pPacket);
        if (ret < 0) {
            if (ret == AVERROR_EOF && g_isLooping) {
                LogToFile("[Decode] LOOP");
                av_seek_frame(g_pFmtCtx, g_vidStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(g_pCodecCtx);
                timingInit = false;
            }
            av_packet_unref(g_pPacket);
            Sleep(5);
            continue;
        }

        if (g_pPacket->stream_index != g_vidStreamIdx) {
            av_packet_unref(g_pPacket);
            continue;
        }

        ret = avcodec_send_packet(g_pCodecCtx, g_pPacket);
        av_packet_unref(g_pPacket);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(g_pCodecCtx, g_pFrame);
        if (ret != 0) continue;

        if (!g_pFrame->data[0]) {
            av_frame_unref(g_pFrame);
            continue;
        }

        // Frame timing
        if (g_pFrame->pts != AV_NOPTS_VALUE) {
            AVStream* s = g_pFmtCtx->streams[g_vidStreamIdx];
            if (s && s->time_base.num > 0) {
                LONGLONG pts = g_pFrame->pts * s->time_base.num * 10000000LL / s->time_base.den;
                if (!timingInit) {
                    startPTS = pts;
                    startWall = GetNow100ns();
                    timingInit = true;
                }
                LONGLONG wait = (pts - startPTS) - (GetNow100ns() - startWall);
                if (wait > 20000LL && wait < 50000000LL) {
                    DWORD sleepMs = (DWORD)(wait / 10000LL);
                    if (sleepMs > 0 && sleepMs < 500) Sleep(sleepMs);
                } else if (wait < -10000000LL) {
                    timingInit = false;
                }
            }
        }

        // Convert
        BYTE* dst = g_pixBuf[g_pixBack];
        if (dst && g_pSwsCtx) {
            uint8_t* d[1] = { dst };
            int ds[1] = { g_pFrame->width * 4 };
            sws_scale(g_pSwsCtx, g_pFrame->data, g_pFrame->linesize, 0, g_pFrame->height, d, ds);
            av_frame_unref(g_pFrame);

            {
                std::lock_guard<std::mutex> lk(g_pixMtx);
                g_pixBack = 1 - g_pixBack;
                g_newFrameReady = true;
            }

            frameCount++;
            InterlockedIncrement(&g_totalFrames);
        } else {
            av_frame_unref(g_pFrame);
        }

        DWORD now = timeGetTime();
        if (now - lastLog > 5000) {
            LogToFile("[Decode] %d frames, %.0f FPS",
                     frameCount, frameCount * 1000.0 / std::max((DWORD)1, now - lastLog));
            lastLog = now;
            frameCount = 0;
        }
    }

    timeEndPeriod(1);
    LogToFile("[Decode] STOPPED - total: %d", g_totalFrames);
    return 0;
}

bool LoadVideo(const wchar_t* path, ID2D1RenderTarget* rt) {
    if (!path || !rt) return false;
    std::lock_guard<std::mutex> lk(g_vidMtx);

    // Stop old thread
    if (g_hDecodeThread) {
        g_decodeRunning = 0;
        g_vidLoaded = false;
        if (g_hFrameConsumed) SetEvent(g_hFrameConsumed);
        WaitForSingleObject(g_hDecodeThread, 3000);
        CloseHandle(g_hDecodeThread);
        g_hDecodeThread = NULL;
    }
    g_newFrameReady = false;

    // Free old
    if (g_pPacket)   { av_packet_free(&g_pPacket);   g_pPacket   = NULL; }
    if (g_pFrame)    { av_frame_free(&g_pFrame);     g_pFrame    = NULL; }
    if (g_pSwsCtx)   { sws_freeContext(g_pSwsCtx);   g_pSwsCtx   = NULL; }
    if (g_pCodecCtx) { avcodec_free_context(&g_pCodecCtx); g_pCodecCtx = NULL; }
    if (g_pFmtCtx)   { avformat_close_input(&g_pFmtCtx); g_pFmtCtx = NULL; }
    if (g_pVideoBmp) { g_pVideoBmp->Release(); g_pVideoBmp = NULL; }
    free(g_pixBuf[0]); g_pixBuf[0] = NULL;
    free(g_pixBuf[1]); g_pixBuf[1] = NULL;
    g_pixBack = 0;
    g_vidStreamIdx = -1;
    g_totalFrames = 0;
    g_vidLoaded = false;

    // Open file
    char path_utf8[MAX_PATH * 4] = {0};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path_utf8, sizeof(path_utf8) - 1, NULL, NULL);

    LogToFile("[Load] Opening: %s", path_utf8);

    int ret = avformat_open_input(&g_pFmtCtx, path_utf8, NULL, NULL);
    if (ret < 0 || !g_pFmtCtx) {
        LogToFile("[ERR] avformat_open_input: %d", ret);
        return false;
    }
    avformat_find_stream_info(g_pFmtCtx, NULL);

    g_vidStreamIdx = -1;
    for (unsigned i = 0; i < g_pFmtCtx->nb_streams; i++) {
        if (g_pFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_vidStreamIdx = i;
            break;
        }
    }
    if (g_vidStreamIdx < 0) {
        LogToFile("[ERR] No video stream");
        avformat_close_input(&g_pFmtCtx);
        return false;
    }

    AVCodecParameters* cp = g_pFmtCtx->streams[g_vidStreamIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(cp->codec_id);
    if (!codec) {
        LogToFile("[ERR] No decoder");
        avformat_close_input(&g_pFmtCtx);
        return false;
    }

    LogToFile("[Load] Codec: %s, %dx%d", codec->name, cp->width, cp->height);

    g_pCodecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(g_pCodecCtx, cp);
    g_pCodecCtx->thread_count = 4;

    ret = avcodec_open2(g_pCodecCtx, codec, NULL);
    if (ret < 0) {
        LogToFile("[ERR] avcodec_open2: %d", ret);
        avformat_close_input(&g_pFmtCtx);
        return false;
    }

    g_srcWidth  = cp->width;
    g_srcHeight = cp->height;
    g_vidW = g_srcWidth;
    g_vidH = g_srcHeight;

    g_pSwsCtx = sws_getContext(g_srcWidth, g_srcHeight, (AVPixelFormat)cp->format,
                               g_vidW, g_vidH, AV_PIX_FMT_BGRA,
                               SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_pSwsCtx) {
        LogToFile("[ERR] sws_getContext failed");
        avformat_close_input(&g_pFmtCtx);
        return false;
    }

    AVRational fr = g_pFmtCtx->streams[g_vidStreamIdx]->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = g_pFmtCtx->streams[g_vidStreamIdx]->r_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) { fr.num = 30; fr.den = 1; }
    g_frameDur = std::max(1, (int)(1000.0 * fr.den / fr.num));

    D2D1_SIZE_U sz = D2D1::SizeU(g_vidW, g_vidH);
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    rt->CreateBitmap(sz, NULL, 0, bp, &g_pVideoBmp);

    UINT bufSize = g_vidW * g_vidH * 4;
    g_pixBuf[0] = (BYTE*)calloc(1, bufSize);
    g_pixBuf[1] = (BYTE*)calloc(1, bufSize);
    g_pixBack = 0;

    g_pFrame  = av_frame_alloc();
    g_pPacket = av_packet_alloc();

    if (!g_hFrameConsumed) g_hFrameConsumed = CreateEvent(NULL, FALSE, TRUE, NULL);

    QueryPerformanceFrequency(&g_qpcFreq);

    g_newFrameReady = false;
    g_vidLoaded = true;
    g_decodeRunning = 1;

    g_hDecodeThread = CreateThread(NULL, 0, DecodeThreadProc, NULL, 0, NULL);

    LogToFile("[Load] OK - %dx%d @ %dfps", g_vidW, g_vidH, 1000 / g_frameDur);
    return true;
}

void StopDecodeThread() {
    g_decodeRunning = 0;
    g_vidLoaded = false;
    if (g_hFrameConsumed) SetEvent(g_hFrameConsumed);

    if (g_hDecodeThread) {
        WaitForSingleObject(g_hDecodeThread, 3000);
        CloseHandle(g_hDecodeThread);
        g_hDecodeThread = NULL;
    }
    g_newFrameReady = false;

    if (g_pPacket)   { av_packet_free(&g_pPacket);   g_pPacket   = NULL; }
    if (g_pFrame)    { av_frame_free(&g_pFrame);     g_pFrame    = NULL; }
    if (g_pSwsCtx)   { sws_freeContext(g_pSwsCtx);   g_pSwsCtx   = NULL; }
    if (g_pCodecCtx) { avcodec_free_context(&g_pCodecCtx); g_pCodecCtx = NULL; }
    if (g_pFmtCtx)   { avformat_close_input(&g_pFmtCtx); g_pFmtCtx = NULL; }
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
    BYTE* src = g_pixBuf[front];
    if (!src) return false;
    D2D1_RECT_U r = D2D1::RectU(0, 0, g_vidW, g_vidH);
    g_pVideoBmp->CopyFromMemory(&r, src, g_vidW * 4);
    if (g_hFrameConsumed) SetEvent(g_hFrameConsumed);
    return true;
}

D2D1_RECT_F VidLetterbox(float sw, float sh) {
    if (!g_vidW || !g_vidH) return D2D1::RectF(0, 0, sw, sh);
    float s = std::min(sw / g_vidW, sh / g_vidH);
    float dw = g_vidW * s, dh = g_vidH * s;
    return D2D1::RectF((sw - dw) * 0.5f, (sh - dh) * 0.5f, (sw + dw) * 0.5f, (sh + dh) * 0.5f);
}
