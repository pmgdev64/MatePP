// main.cpp - FFMPEG VERSION - NO AUDIO
// D2D Wallpaper Engine - Using FFmpeg API for video playback

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <mutex>
#include <shellapi.h>
#include <cstddef>
#include <commctrl.h>
#include <string>
#include <fstream>
#include <cstdarg>
#include <algorithm>
#include <psapi.h>
#include <signal.h>
#include <setjmp.h>
#include <mmsystem.h>
#include <malloc.h>  // FIX/NEW: _resetstkoflw() — cần cho VectoredCrashHandler xử lý EXCEPTION_STACK_OVERFLOW

#include "common.h"
#include "settings.h"
#include "about.h"
#include "pipe_server.h"
#include "config.h"

// KHÔNG #include "audio_system.h" nữa

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "ole32")
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "psapi")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")
// KHÔNG cần swresample.lib nữa
// #pragma comment(lib, "swresample.lib")

// ============================================================
//  System Tray Definitions
// ============================================================
#define WM_TRAYICON (WM_USER + 100)
#define ID_TRAY_EXIT         1001
#define ID_TRAY_SHOW         1002
#define ID_TRAY_HIDE         1003
#define ID_TRAY_RELOAD       1004
#define ID_TRAY_PLAY_PAUSE   1005
#define ID_TRAY_NEXT         1006
#define ID_TRAY_PREV         1007
// #define ID_TRAY_VOLUME_UP    1008  // BỎ
// #define ID_TRAY_VOLUME_DOWN  1009  // BỎ
// #define ID_TRAY_MUTE         1010  // BỎ
#define ID_TRAY_SETTINGS     1011
#define ID_TRAY_ABOUT        1012
#define ID_TRAY_LOOP         1013
#define ID_TRAY_TOP_MOST     1014
#define ID_TRAY_PERFORMANCE  1015
#define ID_TRAY_DEBUG_TOGGLE 1016

#define IDI_MAIN_ICON 101

// ============================================================
//  Forward declarations from video_decoder.cpp
// ============================================================
extern bool LoadVideo(const wchar_t* path, ID2D1RenderTarget* rt);
extern DWORD WINAPI DecodeThread(LPVOID);
extern void StopDecodeThread();
extern bool ReadVideoFrame();
extern D2D1_RECT_F VidLetterbox(float sw, float sh);

// ============================================================
//  Forward declarations
// ============================================================
void LogToFile(const char* msg, ...);
static HWND GetWorkerW();
void ReloadCurrentMedia();
bool LoadMediaByIndex(int index);
bool LoadNextMedia();
bool LoadPrevMedia();

// ============================================================
template<typename T> inline void SafeRelease(T*& p) {
    if(p) {
        p->Release();
        p = NULL;
    }
}

// ============================================================
//  Globals - DEFINITIONS
// ============================================================
ID2D1Factory*          g_pD2DFactory = NULL;
ID2D1HwndRenderTarget* g_pRT         = NULL;
ID2D1SolidColorBrush*  g_pBrush      = NULL;
IDWriteFactory*        g_pDWrite     = NULL;
IDWriteTextFormat*     g_pTextFmt    = NULL;

ID2D1Bitmap*     g_pVideoBmp = NULL;
UINT  g_vidW=0, g_vidH=0;
DWORD g_frameDur=33, g_lastTick=0;
bool  g_vidLoaded=false, g_useVideo=false;
std::mutex g_vidMtx;

// Decode thread
HANDLE g_hDecodeThread   = NULL;
volatile bool g_decodeRunning = false;

// CPU pixel double-buffer - 2 buffers for safety
BYTE* g_pixBuf[2]        = {NULL, NULL};
int   g_pixBack          = 0;
std::mutex   g_pixMtx;
volatile bool g_newFrameReady = false;
HANDLE g_hFrameConsumed = NULL;

// Render target valid flag
bool g_renderTargetValid = true;

// Timing anchor
LONGLONG g_vidStartWall = 0;
LONGLONG g_vidStartTS   = 0;
LARGE_INTEGER g_qpfFreq = {0};

// System Tray globals
NOTIFYICONDATAW g_nid = {0};
bool g_trayIconCreated = false;
HWND g_hMainWnd = NULL;
HWND g_hTrayWnd = NULL;

// Media control globals - KHÔNG CÒN volume/mute
bool g_isPaused = false;
bool g_isLooping = true;
bool g_isTopMost = false;
// float g_volume = 1.0f;          // BỎ
// bool g_isMuted = false;         // BỎ
std::vector<std::wstring> g_playlist;
int g_currentTrack = -1;
std::mutex g_playlistMtx;

// Loading state flag
volatile bool g_isLoading = false;
std::mutex g_loadMtx;

// Exception handling globals
volatile LONG g_exceptionCount = 0;
jmp_buf g_jumpBuffer;
bool g_hasJumpBuffer = false;

// ============================================================
//  DEBUG TEXT GLOBALS
// ============================================================
bool g_showDebugText = false;
IDWriteTextFormat* g_pDebugTextFmt = NULL;
ID2D1SolidColorBrush* g_pDebugBrush = NULL;
int g_frameCount = 0;
double g_fps = 0.0;
double g_lastFPSTime = 0.0;
static ULARGE_INTEGER g_prevCPU = {0};
static ULARGE_INTEGER g_prevWall = {0};
static bool g_cpuInit = false;

// ============================================================
//  RENDER TOGGLE
// ============================================================
static bool g_renderEnabled = true;
static HWND g_hWallpaperWindow = NULL;
static DWORD g_lastPauseTime = 0;
static bool g_needsRecreate = false;

// ============================================================
//  Memory monitoring
// ============================================================
static void GetMemoryUsage(size_t& privateBytes, size_t& workingSet) {
    PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        privateBytes = pmc.PrivateUsage;
        workingSet = pmc.WorkingSetSize;
    }
}

// ============================================================
//  Recreate Render Target
// ============================================================
static bool RecreateRenderTarget() {
    if (!g_hWallpaperWindow || !IsWindow(g_hWallpaperWindow)) {
        LogToFile("[Recreate] Invalid window");
        return false;
    }

    LogToFile("[Recreate] Recreating render target...");

    if (g_pRT) {
        g_pRT->EndDraw();
        SafeRelease(g_pRT);
    }
    SafeRelease(g_pBrush);
    SafeRelease(g_pDebugBrush);
    SafeRelease(g_pVideoBmp);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwp =
        D2D1::HwndRenderTargetProperties(g_hWallpaperWindow, D2D1::SizeU(sw, sh));

    HRESULT hr = g_pD2DFactory->CreateHwndRenderTarget(rtp, hwp, &g_pRT);
    if (FAILED(hr)) {
        LogToFile("[Recreate] CreateHwndRenderTarget failed: 0x%08X", (unsigned)hr);
        g_renderTargetValid = false;
        return false;
    }

    g_pRT->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_pRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &g_pBrush);
    g_pRT->CreateSolidColorBrush(D2D1::ColorF(0, 1, 0, 1), &g_pDebugBrush);
    g_renderTargetValid = true;

    LogToFile("[Recreate] RT recreated successfully (%dx%d)", sw, sh);
    return true;
}

// ============================================================
//  Switch WorkerW
// ============================================================
static void SwitchWorkerW(bool show) {
    if (!g_hWallpaperWindow || !IsWindow(g_hWallpaperWindow)) {
        LogToFile("[Switch] Wallpaper window invalid!");
        return;
    }

    g_renderEnabled = show;

    if (show) {
        ShowWindow(g_hWallpaperWindow, SW_SHOW);
        g_needsRecreate = true;
        LogToFile("[Switch] Wallpaper SHOW");
    } else {
        ShowWindow(g_hWallpaperWindow, SW_HIDE);
        if (g_pVideoBmp) {
            SafeRelease(g_pVideoBmp);
            LogToFile("[Memory] Released video bitmap (hidden)");
        }
        g_renderTargetValid = false;
        LogToFile("[Switch] Wallpaper HIDE");
    }
}

// ============================================================
//  Log functions
// ============================================================
void LogToFile(const char* msg, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, msg);
    vsnprintf(buffer, sizeof(buffer), msg, args);
    va_end(args);

    FILE* f = fopen("wallpaper_log.txt", "a");
    if (f) {
        fprintf(f, "%s\n", buffer);
        fclose(f);
    }
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

// ============================================================
//  Crash report writer — dùng chung giữa SignalHandler (SIGABRT) và
//  VectoredCrashHandler (lỗi phần cứng thật: access violation, v.v.)
// ============================================================
static void WriteCrashReport(const char* reason) {
    FILE* f = fopen("crash_report.txt", "w");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "=== Wallpaper Engine Crash Report ===\n");
        fprintf(f, "Date: %04d-%02d-%02d %02d:%02d:%02d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "Reason: %s\n", reason);
        void* stack[64];
        USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);
        fprintf(f, "Call Stack:\n");
        for (USHORT i = 0; i < frames; i++) {
            fprintf(f, "  #%d: 0x%p\n", i, stack[i]);
        }
        fclose(f);
    }
}

// ============================================================
//  Signal Handlers
// ============================================================
// FIX: signal() của CRT trên Windows KHÔNG đáng tin cậy để bắt lỗi phần
// cứng thật (SIGSEGV/SIGFPE/SIGILL) — đây là hạn chế đã biết của
// MSVCRT/mingw runtime, handler này thường không fire với access
// violation/div-by-zero thật, hoặc fire với state không nhất quán. Chỉ
// còn dùng signal() cho SIGABRT (assert()/abort() thật sự đi qua CRT,
// signal() bắt được đúng) và SIGTERM/SIGINT (điều khiển tiến trình, không
// liên quan lỗi phần cứng). Phần lỗi phần cứng chuyển hẳn sang
// VectoredCrashHandler bên dưới — dùng AddVectoredExceptionHandler, API
// Win32 thuần, hoạt động đúng và đáng tin cậy trên MinGW64 mà không cần
// cú pháp __try/__except (MSVC-only, GCC không hỗ trợ).
static void SignalHandler(int signal) {
    const char* sigName = "UNKNOWN";
    switch(signal) {
        case SIGABRT: sigName = "SIGABRT (Abort/assert)"; break;
        case SIGTERM: sigName = "SIGTERM (Termination)"; break;
        case SIGINT:  sigName = "SIGINT (Interrupt)"; break;
    }

    LogToFile("[SIGNAL] Caught signal: %s (%d)", sigName, signal);
    InterlockedIncrement(&g_exceptionCount);

    WriteCrashReport(sigName);

    if(g_hasJumpBuffer) {
        longjmp(g_jumpBuffer, 1);
    } else {
        exit(1);
    }
}

// ============================================================
//  Vectored Exception Handler — SEH-equivalent thật sự trên MinGW64
// ============================================================
// FIX/NEW: MSVC's __try/__except (SEH) KHÔNG tồn tại trong GCC/MinGW —
// đây là language extension riêng của MSVC, cố dùng sẽ lỗi biên dịch.
// AddVectoredExceptionHandler là API Win32 thuần (không phải compiler
// extension) cho phép bắt đúng loại lỗi mà __try/__except bắt được:
// access violation, stack overflow, divide-by-zero, illegal instruction...
//
// CẢNH BÁO QUAN TRỌNG: MinGW-w64 bản x86_64 dùng chính cơ chế SEH để
// implement C++ exception (throw/catch) — nghĩa là MỌI throw trong code
// (kể cả std::bad_alloc, các catch(...) sẵn có trong LoadMediaByIndex,
// WinMain, v.v.) đều đi qua đúng con đường exception-dispatch này. Nếu
// handler bên dưới can thiệp (log + longjmp) vào MỌI exception code một
// cách vô điều kiện, nó sẽ phá hỏng toàn bộ try/catch(...) hiện có trong
// codebase — mọi throw hợp lệ sẽ bị coi là crash chết người. Vì vậy
// handler CHỈ can thiệp với danh sách exception code là lỗi phần cứng
// thật sự gây crash; mọi thứ khác (bao gồm mã C++ EH 0xE06D7363, các mã
// liên quan debugger) phải trả EXCEPTION_CONTINUE_SEARCH để nhường lại
// cho cơ chế dispatch bình thường xử lý tiếp.
static LONG WINAPI VectoredCrashHandler(PEXCEPTION_POINTERS ExceptionInfo) {
    DWORD code = ExceptionInfo->ExceptionRecord->ExceptionCode;

    const char* reason = NULL;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      reason = "EXCEPTION_ACCESS_VIOLATION"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   reason = "EXCEPTION_ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    reason = "EXCEPTION_INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    reason = "EXCEPTION_FLT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: reason = "EXCEPTION_ARRAY_BOUNDS_EXCEEDED"; break;
        case EXCEPTION_PRIV_INSTRUCTION:      reason = "EXCEPTION_PRIV_INSTRUCTION"; break;
        case EXCEPTION_IN_PAGE_ERROR:         reason = "EXCEPTION_IN_PAGE_ERROR"; break;
        case EXCEPTION_STACK_OVERFLOW: {
            // Stack gần cạn khi vào đây — tránh làm việc nặng (CaptureStackBackTrace/
            // fprintf vẫn tạm chấp nhận được nhờ guard page dự phòng của Windows,
            // nhưng KHÔNG được làm gì phức tạp hơn). Phải gọi _resetstkoflw() để khôi
            // phục guard page trước khi longjmp, nếu không lần overflow tiếp theo sẽ
            // không còn được báo nữa (guard page chỉ tự động kích hoạt lại sau khi gọi
            // hàm này).
            LogToFile("[SEH] EXCEPTION_STACK_OVERFLOW");
            InterlockedIncrement(&g_exceptionCount);
            WriteCrashReport("EXCEPTION_STACK_OVERFLOW");
            _resetstkoflw();
            if (g_hasJumpBuffer) {
                longjmp(g_jumpBuffer, 1);
            } else {
                exit(1);
            }
        }
        default:
            // Không phải lỗi phần cứng mình quan tâm — QUAN TRỌNG: nhường lại
            // cho cơ chế dispatch bình thường (C++ EH, debugger, v.v.), không
            // được nuốt exception ở đây.
            return EXCEPTION_CONTINUE_SEARCH;
    }

    LogToFile("[SEH] Caught: %s (code 0x%08lX) at address 0x%p",
        reason, code, ExceptionInfo->ExceptionRecord->ExceptionAddress);
    InterlockedIncrement(&g_exceptionCount);
    WriteCrashReport(reason);

    if (g_hasJumpBuffer) {
        longjmp(g_jumpBuffer, 1);
        // không return — longjmp không quay lại
    }

    // Không có jump buffer để nhảy về (VD lỗi xảy ra trước khi WinMain vào
    // setjmp) — không thể phục hồi an toàn, để process kết thúc bình thường
    // thay vì cố longjmp vào chỗ không tồn tại.
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InitExceptionHandlers() {
    signal(SIGABRT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGINT, SignalHandler);

    // '1' = gọi handler này TRƯỚC các vectored handler khác (nếu có) và
    // trước frame-based SEH handlers thông thường.
    AddVectoredExceptionHandler(1, VectoredCrashHandler);

    LogToFile("[EXCEPTION] Signal handlers + Vectored Exception Handler registered");
}

// ============================================================
//  GIF Player - OPTIMIZED
// ============================================================
static struct GifPlayer {
    std::vector<ID2D1Bitmap*> frames;
    std::vector<UINT> delays;
    int cur=0;
    DWORD last=0;
    UINT w=0,h=0;
    bool loaded=false;
    std::mutex mtx;
    static const int MAX_FRAMES = 50;

    bool Load(const wchar_t* path, ID2D1RenderTarget* rt) {
        try {
            std::lock_guard<std::mutex> lk(mtx);
            Release();

            IWICImagingFactory* pWIC = NULL;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL,
                                           CLSCTX_INPROC_SERVER,
                                           IID_IWICImagingFactory,
                                           (void**)&pWIC);
            if(FAILED(hr)) return false;

            IWICBitmapDecoder* pDec = NULL;
            hr = pWIC->CreateDecoderFromFilename(path, NULL, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &pDec);
            if(FAILED(hr)) {
                SafeRelease(pWIC);
                return false;
            }

            UINT fc = 0;
            pDec->GetFrameCount(&fc);
            UINT frameToLoad = std::min(fc, (UINT)MAX_FRAMES);

            for(UINT i = 0; i < frameToLoad; i++) {
                IWICBitmapFrameDecode* pF = NULL;
                if(FAILED(pDec->GetFrame(i, &pF))) continue;

                IWICFormatConverter* pC = NULL;
                pWIC->CreateFormatConverter(&pC);
                pC->Initialize(pF, GUID_WICPixelFormat32bppPBGRA,
                              WICBitmapDitherTypeNone, NULL, 0,
                              WICBitmapPaletteTypeMedianCut);

                ID2D1Bitmap* bmp = NULL;
                rt->CreateBitmapFromWicBitmap(pC, NULL, &bmp);

                if(bmp) {
                    frames.push_back(bmp);
                    UINT d = 100;
                    IWICMetadataQueryReader* pM = NULL;
                    if(SUCCEEDED(pF->GetMetadataQueryReader(&pM))) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if(SUCCEEDED(pM->GetMetadataByName(L"/grctlext/Delay", &pv)) &&
                           pv.vt == VT_UI2) {
                            d = pv.uiVal * 10;
                            if(d < 20) d = 100;
                        }
                        PropVariantClear(&pv);
                        SafeRelease(pM);
                    }
                    delays.push_back(d);
                }
                SafeRelease(pC);
                SafeRelease(pF);
            }

            SafeRelease(pDec);
            SafeRelease(pWIC);

            if(!frames.empty()) {
                D2D1_SIZE_F s = frames[0]->GetSize();
                w = (UINT)s.width;
                h = (UINT)s.height;
            }

            loaded = !frames.empty();
            if(loaded) {
                last = GetTickCount();
                LogToFile("[GIF] Loaded %zu frames", frames.size());
            }
            return loaded;
        } catch(...) {
            LogToFile("[EXCEPTION] GifPlayer::Load");
            InterlockedIncrement(&g_exceptionCount);
            return false;
        }
    }

    void Update() {
        try {
            std::lock_guard<std::mutex> lk(mtx);
            if(!loaded || frames.empty() || g_isPaused) return;

            DWORD now = GetTickCount();
            if(cur < 0 || cur >= (int)frames.size()) cur = 0;
            if(now - last >= delays[cur]) {
                cur = (cur + 1) % (int)frames.size();
                last = now;
            }
        } catch(...) {
            LogToFile("[EXCEPTION] GifPlayer::Update");
            InterlockedIncrement(&g_exceptionCount);
        }
    }

    void Draw(ID2D1RenderTarget* rt, D2D1_RECT_F d) {
        try {
            std::lock_guard<std::mutex> lk(mtx);
            if(!loaded || !rt) return;
            if(cur < 0 || cur >= (int)frames.size()) cur = 0;
            ID2D1Bitmap* b = frames[cur];
            if(b) rt->DrawBitmap(b, d, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } catch(...) {
            LogToFile("[EXCEPTION] GifPlayer::Draw");
            InterlockedIncrement(&g_exceptionCount);
        }
    }

    D2D1_RECT_F Letterbox(float sw, float sh) const {
        if(!w || !h) return D2D1::RectF(0, 0, sw, sh);
        float sx = sw / w, sy = sh / h, sc = (sx < sy) ? sx : sy;
        float dw = w * sc, dh = h * sc;
        return D2D1::RectF((sw - dw) * .5f, (sh - dh) * .5f,
                          (sw + dw) * .5f, (sh + dh) * .5f);
    }

    void Release() {
        try {
            std::lock_guard<std::mutex> lk(mtx);
            for(auto& f : frames) SafeRelease(f);
            frames.clear();
            delays.clear();
            loaded = false;
        } catch(...) {
            LogToFile("[EXCEPTION] GifPlayer::Release");
            InterlockedIncrement(&g_exceptionCount);
        }
    }
} g_gif;

static HWND g_hWorker = NULL;
static volatile bool g_running = true;
static float g_time = 0;
static std::mutex g_renderMtx;
static bool g_comInit = false;

// ============================================================
//  Helper Functions
// ============================================================
static bool IsVideoFile(const wchar_t* path) {
    if(!path) return false;
    const wchar_t* exts[] = {L".mp4", L".avi", L".wmv", L".mkv",
                             L".mov", L".webm", L".m4v", L".flv",
                             L".ts", L".m2ts", L".3gp", L".ogg", L".ogv"};
    wchar_t ext[16] = {0};
    const wchar_t* d = wcsrchr(path, L'.');
    if(!d) return false;
    wcscpy_s(ext, d);
    CharLowerW(ext);
    for(int i = 0; i < 13; i++) {
        if(wcscmp(ext, exts[i]) == 0) return true;
    }
    return false;
}

// ============================================================
//  System Tray Functions
// ============================================================
static bool CreateTrayIcon(HWND hwnd) {
    if (g_trayIconCreated) return true;

    g_hMainWnd = hwnd;

    HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MAIN_ICON));
    if (!hIcon) {
        LogToFile("[WARN] Cannot load icon from resource, using default");
        hIcon = LoadIcon(NULL, IDI_APPLICATION);
    } else {
        LogToFile("[OK] Icon loaded from resource successfully");
    }

    ZeroMemory(&g_nid, sizeof(NOTIFYICONDATAW));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = hIcon;
    wcscpy_s(g_nid.szTip, L"Mate++ Lightweight");

    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayIconCreated = true;
        LogToFile("[OK] System tray icon created");
        return true;
    }

    LogToFile("[ERR] Failed to create tray icon");
    if (hIcon) DestroyIcon(hIcon);
    return false;
}

void UpdateTrayTooltip(const wchar_t* tip) {
    if (!g_trayIconCreated) return;
    wcscpy_s(g_nid.szTip, tip);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void RemoveTrayIcon() {
    if (!g_trayIconCreated) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayIconCreated = false;
    LogToFile("[OK] System tray icon removed");
}

// ============================================================
//  Playlist Management
// ============================================================
void ScanPlaylist(const wchar_t* directory) {
    try {
        std::lock_guard<std::mutex> lk(g_playlistMtx);
        g_playlist.clear();
        g_currentTrack = -1;

        wchar_t searchPath[MAX_PATH];
        wcscpy(searchPath, directory);
        wcscat(searchPath, L"*.*");

        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW(searchPath, &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    const wchar_t* ext = wcsrchr(findData.cFileName, L'.');
                    if (ext) {
                        wchar_t extLower[16];
                        wcscpy_s(extLower, ext);
                        CharLowerW(extLower);

                        const wchar_t* videoExts[] = {
                            L".mp4", L".avi", L".wmv", L".mkv",
                            L".mov", L".webm", L".m4v", L".gif",
                            L".flv", L".ts", L".m2ts", L".3gp", L".ogg", L".ogv"
                        };

                        for (int i = 0; i < 14; i++) {
                            if (wcscmp(extLower, videoExts[i]) == 0) {
                                wchar_t fullPath[MAX_PATH];
                                wcscpy(fullPath, directory);
                                wcscat(fullPath, findData.cFileName);
                                g_playlist.push_back(fullPath);
                                break;
                            }
                        }
                    }
                }
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }

        if (!g_playlist.empty()) {
            g_currentTrack = 0;
            LogToFile("[Playlist] Found %zu media files", g_playlist.size());
        }
    } catch(...) {
        LogToFile("[EXCEPTION] ScanPlaylist");
        InterlockedIncrement(&g_exceptionCount);
    }
}

// ============================================================
//  LoadMediaByIndex - KHÔNG CÒN AUDIO
// ============================================================
bool LoadMediaByIndex(int index) {
    if (g_isLoading) {
        LogToFile("[LoadMedia] Already loading, skipping...");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_loadMtx);
    g_isLoading = true;

    try {
        if (index < 0 || index >= (int)g_playlist.size()) {
            LogToFile("[LoadMedia] Invalid index: %d", index);
            g_isLoading = false;
            return false;
        }

        const std::wstring& path = g_playlist[index];
        LogToFile("[LoadMedia] Loading (%d/%zu): %S", index + 1, g_playlist.size(), path.c_str());

        {
            StopDecodeThread();
            // KHÔNG CÒN AudioSystem_Stop()

            std::lock_guard<std::mutex> renderLock(g_renderMtx);

            SafeRelease(g_pVideoBmp);
            free(g_pixBuf[0]); g_pixBuf[0] = NULL;
            free(g_pixBuf[1]); g_pixBuf[1] = NULL;

            g_gif.Release();
            g_vidLoaded = false;
            g_useVideo  = false;
            g_isPaused  = false;
        }

        bool loadSuccess = false;

        if (!g_pRT || !g_renderTargetValid) {
            LogToFile("[ERR] LoadMediaByIndex: g_pRT invalid!");
            g_isLoading = false;
            return false;
        }

        if (IsVideoFile(path.c_str())) {
            g_useVideo = true;
            loadSuccess = LoadVideo(path.c_str(), g_pRT);
            if (!loadSuccess) {
                LogToFile("[LoadMedia] Video load failed, trying as GIF...");
                g_useVideo = false;
                loadSuccess = g_gif.Load(path.c_str(), g_pRT);
            }
        } else {
            g_useVideo = false;
            loadSuccess = g_gif.Load(path.c_str(), g_pRT);
        }

        if (loadSuccess) {
            // FIX/NEW: đây là điểm chốt duy nhất mọi đường load (startup,
            // Next/Prev, pipe LOAD) đều đi qua — cập nhật path "đang áp
            // dụng" ở đây thay vì rải rác ở từng caller, đảm bảo luôn khớp
            // thực tế bất kể do đâu kích hoạt việc đổi wallpaper.
            wcscpy_s(g_currentWallpaperPath, path.c_str());

            wchar_t tip[128];
            const wchar_t* filename = wcsrchr(path.c_str(), L'\\');
            if (g_useVideo && g_vidLoaded) {
                swprintf_s(tip, L"▶ %s", filename ? filename + 1 : path.c_str());
                // KHÔNG CÒN AUDIO
            } else if (g_gif.loaded) {
                swprintf_s(tip, L"▶ %s", filename ? filename + 1 : path.c_str());
            } else {
                wcscpy_s(tip, L"Mate++ Lightweight");
            }
            UpdateTrayTooltip(tip);
            LogToFile("[LoadMedia] SUCCESS");
        } else {
            LogToFile("[LoadMedia] FAILED");
            UpdateTrayTooltip(L"⚠ Load Failed");
        }

        g_isLoading = false;
        return loadSuccess;

    } catch(...) {
        LogToFile("[EXCEPTION] LoadMediaByIndex");
        InterlockedIncrement(&g_exceptionCount);
        g_isLoading = false;
        return false;
    }
}

bool LoadNextMedia() {
    if (g_isLoading) {
        LogToFile("[Next] Already loading, skipping...");
        return false;
    }

    std::lock_guard<std::mutex> lk(g_playlistMtx);
    if (g_playlist.empty() || g_currentTrack < 0) {
        LogToFile("[Next] Playlist empty or invalid");
        return false;
    }

    int newIndex = (g_currentTrack + 1) % g_playlist.size();
    LogToFile("[Next] Track %d -> %d", g_currentTrack, newIndex);
    g_currentTrack = newIndex;

    return LoadMediaByIndex(g_currentTrack);
}

bool LoadPrevMedia() {
    if (g_isLoading) {
        LogToFile("[Prev] Already loading, skipping...");
        return false;
    }

    std::lock_guard<std::mutex> lk(g_playlistMtx);
    if (g_playlist.empty() || g_currentTrack < 0) {
        LogToFile("[Prev] Playlist empty or invalid");
        return false;
    }

    int newIndex = (g_currentTrack - 1 + g_playlist.size()) % g_playlist.size();
    LogToFile("[Prev] Track %d -> %d", g_currentTrack, newIndex);
    g_currentTrack = newIndex;

    return LoadMediaByIndex(g_currentTrack);
}

void ReloadCurrentMedia() {
    if (g_isLoading) {
        LogToFile("[Reload] Already loading, skipping...");
        return;
    }

    LogToFile("[Reload] Reloading current media...");

    std::lock_guard<std::mutex> lock(g_loadMtx);
    g_isLoading = true;

    try {
        {
            StopDecodeThread();
            // KHÔNG CÒN AudioSystem_Stop()

            std::lock_guard<std::mutex> renderLock(g_renderMtx);
            SafeRelease(g_pVideoBmp);
            free(g_pixBuf[0]); g_pixBuf[0] = NULL;
            free(g_pixBuf[1]); g_pixBuf[1] = NULL;
            g_gif.Release();
            g_vidLoaded = false;
            g_useVideo  = false;
            g_isPaused  = false;
        }

        if (!g_pRT || !g_renderTargetValid) {
            LogToFile("[ERR] ReloadCurrentMedia: g_pRT invalid!");
            g_isLoading = false;
            return;
        }

        if (!g_playlist.empty() && g_currentTrack >= 0 && g_currentTrack < (int)g_playlist.size()) {
            const std::wstring& path = g_playlist[g_currentTrack];
            LogToFile("[Reload] Reloading: %S", path.c_str());

            bool loadSuccess = false;
            if (IsVideoFile(path.c_str())) {
                g_useVideo = true;
                loadSuccess = LoadVideo(path.c_str(), g_pRT);
                if (!loadSuccess) {
                    g_useVideo = false;
                    loadSuccess = g_gif.Load(path.c_str(), g_pRT);
                }
            } else {
                g_useVideo = false;
                loadSuccess = g_gif.Load(path.c_str(), g_pRT);
            }

            if (loadSuccess) {
                LogToFile("[Reload] SUCCESS");
                wchar_t tip[128];
                const wchar_t* filename = wcsrchr(path.c_str(), L'\\');
                if (g_useVideo && g_vidLoaded) {
                    swprintf_s(tip, L"▶ %s", filename ? filename + 1 : path.c_str());
                    // KHÔNG CÒN AUDIO
                } else if (g_gif.loaded) {
                    swprintf_s(tip, L"▶ %s", filename ? filename + 1 : path.c_str());
                } else {
                    wcscpy_s(tip, L"Mate++ Lightweight");
                }
                UpdateTrayTooltip(tip);
            } else {
                LogToFile("[Reload] FAILED");
                UpdateTrayTooltip(L"⚠ Reload Failed");
            }
        } else {
            LogToFile("[Reload] No media to reload");
            UpdateTrayTooltip(L"⚠ No Media");
        }
    } catch(...) {
        LogToFile("[EXCEPTION] ReloadCurrentMedia");
        InterlockedIncrement(&g_exceptionCount);
    }

    g_isLoading = false;
}

// ============================================================
//  Context Menu - KHÔNG CÒN AUDIO
// ============================================================
static void ShowContextMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING | (g_renderEnabled ? MF_CHECKED : 0), ID_TRAY_SHOW, L"Show Wallpaper");
    AppendMenuW(hMenu, MF_STRING | (!g_renderEnabled ? MF_CHECKED : 0), ID_TRAY_HIDE, L"Hide Wallpaper");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    HMENU hMediaMenu = CreatePopupMenu();
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_PLAY_PAUSE,
                g_isPaused ? L"Play" : L"Pause");
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_NEXT, L"Next Track");
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_PREV, L"Previous Track");
    AppendMenuW(hMediaMenu, MF_SEPARATOR, 0, NULL);
    // BỎ VOLUME và MUTE
    // AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_VOLUME_UP, L"Volume +");
    // AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_VOLUME_DOWN, L"Volume -");
    // AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_MUTE, ...);
    AppendMenuW(hMediaMenu, MF_STRING | (g_isLooping ? MF_CHECKED : 0),
                ID_TRAY_LOOP, L"Loop");

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hMediaMenu, L"Media Control");

    HMENU hDisplayMenu = CreatePopupMenu();
    AppendMenuW(hDisplayMenu, MF_STRING | (g_isTopMost ? MF_CHECKED : 0),
                ID_TRAY_TOP_MOST, L"Always on Top");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hDisplayMenu, L"Display");

    HMENU hPerfMenu = CreatePopupMenu();
    AppendMenuW(hPerfMenu, MF_STRING | (g_showDebugText ? MF_CHECKED : 0),
                ID_TRAY_DEBUG_TOGGLE, L"Show Debug Text");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hPerfMenu, L"Performance");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_RELOAD, L"Reload Media");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Settings");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_ABOUT, L"About");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, hwnd, NULL);

    DestroyMenu(hMenu);

    switch (cmd) {
        case ID_TRAY_SHOW:
            g_renderEnabled = true;
            SwitchWorkerW(true);
            LogToFile("[Tray] Wallpaper SHOW");
            UpdateTrayTooltip(L"Mate++ Lightweight");
            break;

        case ID_TRAY_HIDE:
            g_renderEnabled = false;
            SwitchWorkerW(false);
            LogToFile("[Tray] Wallpaper HIDE");
            UpdateTrayTooltip(L"Mate++ (Hidden)");
            break;

        case ID_TRAY_PLAY_PAUSE:
            g_isPaused = !g_isPaused;
            LogToFile("[Media] %s", g_isPaused ? "Paused" : "Resumed");
            UpdateTrayTooltip(g_isPaused ? L"Paused" : L"Playing");
            break;

        case ID_TRAY_NEXT:
            LogToFile("[Media] Next track");
            LoadNextMedia();
            break;

        case ID_TRAY_PREV:
            LogToFile("[Media] Previous track");
            LoadPrevMedia();
            break;

        // BỎ CASE VOLUME và MUTE
        // case ID_TRAY_VOLUME_UP: ...
        // case ID_TRAY_VOLUME_DOWN: ...
        // case ID_TRAY_MUTE: ...

        case ID_TRAY_LOOP:
            g_isLooping = !g_isLooping;
            LogToFile("[Media] Loop: %s", g_isLooping ? "ON" : "OFF");
            Config_Save();
            break;

        case ID_TRAY_TOP_MOST:
            g_isTopMost = !g_isTopMost;
            LogToFile("[Display] Always on Top: %s", g_isTopMost ? "ON" : "OFF");
            if (g_hWorker && IsWindow(g_hWorker)) {
                SetWindowPos(g_hWorker, g_isTopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            Config_Save();
            break;

        case ID_TRAY_RELOAD:
            LogToFile("[Tray] Reload media");
            ReloadCurrentMedia();
            break;

        case ID_TRAY_SETTINGS:
            LogToFile("[Tray] Settings");
            ShowSettingsDialog(g_hMainWnd);
            break;

        case ID_TRAY_ABOUT: {
            LogToFile("[Tray] About");
            AboutDialog::Show(hwnd);
            break;
        }

        case ID_TRAY_DEBUG_TOGGLE:
            g_showDebugText = !g_showDebugText;
            LogToFile("[Debug] Show Debug Text: %s", g_showDebugText ? "ON" : "OFF");
            break;

        case ID_TRAY_EXIT:
            LogToFile("[Tray] Exit");
            g_running = false;
            PostQuitMessage(0);
            break;
    }
}

static void HandleTrayMessage(WPARAM wParam, LPARAM lParam) {
    try {
        if (wParam != 1) return;

        switch (LOWORD(lParam)) {
            case WM_LBUTTONDBLCLK:
            case WM_LBUTTONDOWN:
                g_renderEnabled = !g_renderEnabled;
                SwitchWorkerW(g_renderEnabled);
                LogToFile("[Tray] Toggle render: %s", g_renderEnabled ? "ON" : "OFF");
                UpdateTrayTooltip(g_renderEnabled ? L"Mate++ Lightweight" : L"Mate++ (Hidden)");
                break;

            case WM_RBUTTONDOWN:
                ShowContextMenu(g_hMainWnd);
                break;
        }
    } catch(...) {
        LogToFile("[EXCEPTION] HandleTrayMessage");
        InterlockedIncrement(&g_exceptionCount);
    }
}

// ============================================================
//  Window Procedure
// ============================================================
static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    try {
        switch (message) {
            case WM_TRAYICON:
                HandleTrayMessage(wParam, lParam);
                return 0;
            case WM_DESTROY:
                RemoveTrayIcon();
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
        }
    } catch(...) {
        LogToFile("[EXCEPTION] WndProc");
        InterlockedIncrement(&g_exceptionCount);
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

static HWND CreateHiddenWindow() {
    try {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"MatePP_TrayClass";
        wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MAIN_ICON));

        if (!RegisterClassExW(&wc)) {
            LogToFile("[ERR] Failed to register window class");
            return NULL;
        }

        HWND hWnd = CreateWindowExW(
            0,
            L"MatePP_TrayClass",
            L"Mate++ Lightweight",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            100, 100,
            NULL, NULL,
            wc.hInstance,
            NULL
        );

        return hWnd;
    } catch(...) {
        LogToFile("[EXCEPTION] CreateHiddenWindow");
        InterlockedIncrement(&g_exceptionCount);
        return NULL;
    }
}

// ============================================================
//  GetWorkerW
// ============================================================
static HWND GetWorkerW() {
    try {
        HWND hP = FindWindowW(L"Progman", NULL);
        if(!hP) {
            LogToFile("[GetWorkerW] Progman not found, using desktop");
            return GetDesktopWindow();
        }

        SendMessageTimeoutW(hP, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL);

        HWND hW = NULL;
        EnumWindows([](HWND h, LPARAM l)->BOOL{
            if(FindWindowExW(h, NULL, L"SHELLDLL_DefView", NULL)) {
                HWND worker = FindWindowExW(NULL, h, L"WorkerW", NULL);
                if(worker) {
                    *(HWND*)l = worker;
                    return FALSE;
                }
            }
            return TRUE;
        }, (LPARAM)&hW);

        if(!hW) {
            LogToFile("[GetWorkerW] WorkerW not found, waiting...");
            for(int i = 0; i < 10 && !hW; i++) {
                Sleep(200);
                if(i == 5) SendMessageTimeoutW(hP, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL);
                EnumWindows([](HWND h, LPARAM l)->BOOL{
                    if(FindWindowExW(h, NULL, L"SHELLDLL_DefView", NULL)) {
                        HWND worker = FindWindowExW(NULL, h, L"WorkerW", NULL);
                        if(worker) {
                            *(HWND*)l = worker;
                            return FALSE;
                        }
                    }
                    return TRUE;
                }, (LPARAM)&hW);
            }
        }

        if(!hW) {
            LogToFile("[GetWorkerW] Using Progman as fallback");
            return hP;
        }

        LogToFile("[GetWorkerW] Found WorkerW: 0x%p", (void*)hW);
        return hW;
    } catch(...) {
        LogToFile("[EXCEPTION] GetWorkerW");
        InterlockedIncrement(&g_exceptionCount);
        return GetDesktopWindow();
    }
}

// ============================================================
//  InitD2D
// ============================================================
static bool InitD2D(HWND hwnd) {
    try {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if(FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            LogToFile("[ERR] COM: 0x%08X", (unsigned)hr);
            return false;
        }
        g_comInit = true;

        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory);
        if(FAILED(hr)) {
            LogToFile("[ERR] D2D1: 0x%08X", (unsigned)hr);
            return false;
        }

        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_pDWrite);
        if(g_pDWrite) {
            g_pDWrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 22, L"en-us", &g_pTextFmt);
            if(g_pTextFmt) {
                g_pTextFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_pTextFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (g_pDWrite) {
            g_pDWrite->CreateTextFormat(L"Consolas", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 12, L"en-us", &g_pDebugTextFmt);
            if (g_pDebugTextFmt) {
                g_pDebugTextFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                g_pDebugTextFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }

        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwp = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(sw, sh));

        hr = g_pD2DFactory->CreateHwndRenderTarget(rtp, hwp, &g_pRT);
        if(FAILED(hr)) {
            LogToFile("[ERR] RT: 0x%08X", (unsigned)hr);
            return false;
        }

        g_pRT->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &g_pBrush);
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(0, 1, 0, 1), &g_pDebugBrush);

        g_hWallpaperWindow = g_pRT->GetHwnd();
        g_renderTargetValid = true;
        LogToFile("[OK] D2D ready (%dx%d), wallpaper window: 0x%p", sw, sh, (void*)g_hWallpaperWindow);

        g_renderEnabled = true;
        SwitchWorkerW(true);

        return true;
    } catch(...) {
        LogToFile("[EXCEPTION] InitD2D");
        InterlockedIncrement(&g_exceptionCount);
        return false;
    }
}

// ============================================================
//  RenderFrame - KHÔNG CÒN AUDIO
// ============================================================
static void RenderFrame() {
    try {
        if(!g_pRT || !g_renderTargetValid) {
            if (g_needsRecreate && g_renderEnabled) {
                if (RecreateRenderTarget()) {
                    g_needsRecreate = false;
                    LogToFile("[Render] RT recreated");
                }
            }
            return;
        }

        if (g_isPaused) {
            if (g_lastPauseTime == 0) g_lastPauseTime = GetTickCount();
            if (GetTickCount() - g_lastPauseTime > 10000 && g_pVideoBmp) {
                SafeRelease(g_pVideoBmp);
                LogToFile("[Memory] Released video bitmap (paused >10s)");
            }
        } else {
            g_lastPauseTime = 0;
        }

        if(g_useVideo && g_vidLoaded) {
            if(!g_pVideoBmp && g_vidW > 0 && g_vidH > 0) {
                D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
                );
                D2D1_SIZE_U size = D2D1::SizeU(g_vidW, g_vidH);
                HRESULT hr = g_pRT->CreateBitmap(size, NULL, 0, props, &g_pVideoBmp);
                if (FAILED(hr)) {
                    LogToFile("[ERR] CreateBitmap failed: 0x%08X", (unsigned)hr);
                } else {
                    LogToFile("[OK] Dynamic g_pVideoBmp created (%dx%d)", g_vidW, g_vidH);
                }
            }

            ReadVideoFrame();
        }

        g_renderMtx.lock();

        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        if(!sw || !sh) {
            g_renderMtx.unlock();
            return;
        }

        if(!g_renderEnabled) {
            g_pRT->BeginDraw();
            g_pRT->Clear(D2D1::ColorF(0, 0, 0, 0));
            g_pRT->EndDraw();
            g_renderMtx.unlock();
            return;
        }

        g_pRT->BeginDraw();
        g_pRT->Clear(D2D1::ColorF(0, 0, 0, 1));

        if(g_useVideo && g_vidLoaded && g_pVideoBmp) {
            D2D1_RECT_F rect = VidLetterbox((float)sw, (float)sh);
            g_pRT->DrawBitmap(g_pVideoBmp, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else if(g_gif.loaded) {
            g_gif.Update();
            g_gif.Draw(g_pRT, g_gif.Letterbox((float)sw, (float)sh));
        } else if(g_isLoading) {
            float cx = sw * .5f, cy = sh * .5f;
            g_pBrush->SetColor(D2D1::ColorF(1, 1, 1, 0.7f));
            const wchar_t* loadMsg = L"Loading...";
            g_pRT->DrawText(loadMsg, (UINT)wcslen(loadMsg), g_pTextFmt,
                           D2D1::RectF(cx - 300, cy - 60, cx + 300, cy - 20), g_pBrush);

            float barW = 420.f, barH = 6.f;
            float barX = cx - barW * .5f, barY = cy - barH * .5f;
            g_pBrush->SetColor(D2D1::ColorF(1, 1, 1, 0.12f));
            g_pRT->FillRectangle(D2D1::RectF(barX, barY, barX + barW, barY + barH), g_pBrush);

            float shimW = barW * 0.35f;
            float phase = fmodf(g_time * 0.9f, 1.0f);
            float shimX = barX + (barW + shimW) * phase - shimW;
            float shimL = std::max(shimX, barX);
            float shimR = std::min(shimX + shimW, barX + barW);
            if (shimR > shimL) {
                g_pBrush->SetColor(D2D1::ColorF(0.3f, 0.7f, 1.0f, 0.9f));
                g_pRT->FillRectangle(D2D1::RectF(shimL, barY, shimR, barY + barH), g_pBrush);
            }
        } else {
            float cx = sw * .5f, cy = sh * .5f;
            g_pBrush->SetColor(D2D1::ColorF(1, 1, 1, .3f));
            const wchar_t* m2 = L"Mate++ Lightweight";
            g_pRT->DrawText(m2, (UINT)wcslen(m2), g_pTextFmt,
                D2D1::RectF(cx - 200, cy - 30, cx + 200, cy + 30), g_pBrush);

            g_pBrush->SetColor(D2D1::ColorF(1, 1, 1, .2f));
            const wchar_t* m3 = L"No media loaded";
            g_pRT->DrawText(m3, (UINT)wcslen(m3), g_pTextFmt,
                D2D1::RectF(cx - 150, cy + 20, cx + 150, cy + 50), g_pBrush);
        }

        if (g_showDebugText && g_pDebugTextFmt && g_pDebugBrush) {
            g_frameCount++;
            double now = (double)GetTickCount() / 1000.0;
            if (now - g_lastFPSTime >= 1.0) {
                g_fps = g_frameCount / (now - g_lastFPSTime);
                g_frameCount = 0;
                g_lastFPSTime = now;
            }

            double cpuUsage = 0.0;
            FILETIME creation, exit, kernel, user;
            if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
                ULARGE_INTEGER k, u;
                k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
                u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
                ULARGE_INTEGER curCPU;
                curCPU.QuadPart = k.QuadPart + u.QuadPart;
                ULARGE_INTEGER curWall;
                GetSystemTimeAsFileTime((FILETIME*)&curWall);
                if (g_cpuInit) {
                    LONGLONG cpuDelta = curCPU.QuadPart - g_prevCPU.QuadPart;
                    LONGLONG wallDelta = curWall.QuadPart - g_prevWall.QuadPart;
                    if (wallDelta > 0) {
                        cpuUsage = (double)cpuDelta / (double)wallDelta * 100.0;
                        if (cpuUsage > 100.0) cpuUsage = 100.0;
                        if (cpuUsage < 0.0) cpuUsage = 0.0;
                    }
                } else {
                    g_cpuInit = true;
                }
                g_prevCPU = curCPU;
                g_prevWall = curWall;
            }

            size_t privateBytes = 0, workingSet = 0;
            GetMemoryUsage(privateBytes, workingSet);

            wchar_t debugStr[512];
            swprintf_s(debugStr,
                L"FPS: %.1f\nCPU: %.1f%%\nRAM: %.1f MB\nRes: %dx%d\nVideo: %s\nPaused: %s\nRender: %s\nRT: %s",
                g_fps, cpuUsage,
                privateBytes / (1024.0 * 1024.0),
                g_useVideo ? g_vidW : g_gif.w, g_useVideo ? g_vidH : g_gif.h,
                g_vidLoaded ? (g_useVideo ? L"Video" : L"GIF") : L"None",
                g_isPaused ? L"Yes" : L"No",
                g_renderEnabled ? L"ON" : L"OFF",
                g_renderTargetValid ? L"OK" : L"INVALID");

            D2D1_RECT_F bgRect = D2D1::RectF(8.0f, 8.0f, 380.0f, 210.0f);
            g_pDebugBrush->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));
            g_pRT->FillRectangle(bgRect, g_pDebugBrush);
            g_pDebugBrush->SetColor(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f));
            g_pRT->DrawText(debugStr, (UINT)wcslen(debugStr), g_pDebugTextFmt,
                           D2D1::RectF(12.0f, 12.0f, 380.0f, 210.0f), g_pDebugBrush);
        }

        HRESULT hr = g_pRT->EndDraw();
        g_renderMtx.unlock();

        if(FAILED(hr)) {
            LogToFile("[Render] EndDraw failed: 0x%08X", (unsigned)hr);
            if (hr == D2DERR_RECREATE_TARGET || hr == D2DERR_INVALID_TARGET) {
                g_renderTargetValid = false;
                g_needsRecreate = true;
                LogToFile("[Render] Target invalid, scheduling recreate");
            }
        }
    } catch(...) {
        LogToFile("[EXCEPTION] RenderFrame");
        InterlockedIncrement(&g_exceptionCount);
        g_renderMtx.unlock();
    }
}

// ============================================================
//  Render Thread
// ============================================================
static DWORD WINAPI RenderThread(LPVOID) {
    try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        LARGE_INTEGER f, p, n;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&p);

        HANDLE hTimer = CreateWaitableTimer(NULL, FALSE, NULL);
        timeBeginPeriod(1);

        LARGE_INTEGER frameStart;
        QueryPerformanceCounter(&frameStart);

        while(g_running) {
            QueryPerformanceCounter(&n);
            g_time += (float)((double)(n.QuadPart - p.QuadPart) / f.QuadPart);
            p = n;

            RenderFrame();

            LARGE_INTEGER frameEnd;
            QueryPerformanceCounter(&frameEnd);
            double elapsedMs = (double)(frameEnd.QuadPart - frameStart.QuadPart) * 1000.0 / f.QuadPart;
            double remainMs = 16.67 - elapsedMs;

            if(hTimer && remainMs > 0.5) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(remainMs * 10000.0);
                SetWaitableTimer(hTimer, &due, 0, NULL, NULL, FALSE);
                WaitForSingleObject(hTimer, (DWORD)remainMs + 5);
            } else if(remainMs > 0.1) {
                Sleep((DWORD)remainMs);
            }

            QueryPerformanceCounter(&frameStart);
        }

        if(hTimer) CloseHandle(hTimer);
        timeEndPeriod(1);

        return 0;
    } catch(...) {
        LogToFile("[EXCEPTION] RenderThread");
        InterlockedIncrement(&g_exceptionCount);
        return 1;
    }
}

// ============================================================
//  Cleanup - KHÔNG CÒN AUDIO
// ============================================================
static void Cleanup() {
    try {
        PipeServer_Stop();
        LogToFile("[..] Cleaning up...");

        CloseSettingsDialog();
        // KHÔNG CÒN AudioSystem_Stop()

        if (g_hWallpaperWindow && IsWindow(g_hWallpaperWindow)) {
            SetParent(g_hWallpaperWindow, GetDesktopWindow());
            ShowWindow(g_hWallpaperWindow, SW_HIDE);
            LogToFile("[Cleanup] Wallpaper window detached");
        }

        StopDecodeThread();
        RemoveTrayIcon();

        g_renderMtx.lock();
        g_gif.Release();
        SafeRelease(g_pVideoBmp);
        free(g_pixBuf[0]); g_pixBuf[0] = NULL;
        free(g_pixBuf[1]); g_pixBuf[1] = NULL;

        SafeRelease(g_pBrush);
        SafeRelease(g_pTextFmt);
        SafeRelease(g_pDWrite);
        SafeRelease(g_pRT);
        SafeRelease(g_pD2DFactory);
        SafeRelease(g_pDebugBrush);
        SafeRelease(g_pDebugTextFmt);
        g_renderTargetValid = false;
        g_renderMtx.unlock();

        if (g_hFrameConsumed) {
            CloseHandle(g_hFrameConsumed);
            g_hFrameConsumed = NULL;
        }

        if(g_hWorker && IsWindow(g_hWorker)) {
            InvalidateRect(g_hWorker, NULL, TRUE);
            UpdateWindow(g_hWorker);
        }

        if(g_hMainWnd && IsWindow(g_hMainWnd)) {
            DestroyWindow(g_hMainWnd);
        }

        SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, NULL, SPIF_UPDATEINIFILE);
        LogToFile("[OK] Cleanup complete");
        LogToFile("[EXCEPTION] Total exceptions caught: %d", g_exceptionCount);
    } catch(...) {
        LogToFile("[EXCEPTION] Cleanup");
        InterlockedIncrement(&g_exceptionCount);
    }
}

// ============================================================
//  WinMain
// ============================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    InitExceptionHandlers();

    g_hasJumpBuffer = true;
    if(setjmp(g_jumpBuffer) == 0) {
        try {
            LogToFile("===================================================");
            LogToFile("Mate++ Lightweight v2.0 (FFmpeg) Starting...");
            LogToFile("===================================================");

            int argc;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

            g_hTrayWnd = CreateHiddenWindow();
            if(!g_hTrayWnd) {
                LogToFile("[WARN] Cannot create tray window");
            }

            g_hWorker = GetWorkerW();
            if(!g_hWorker) {
                LogToFile("[ERR] WorkerW not found");
                if(g_hTrayWnd) DestroyWindow(g_hTrayWnd);
                return 1;
            }
            LogToFile("[OK] WorkerW: 0x%p", (void*)g_hWorker);

            if(!InitD2D(g_hWorker)) {
                LogToFile("[ERR] D2D initialization failed");
                if(g_hTrayWnd) DestroyWindow(g_hTrayWnd);
                return 1;
            }

            if(g_hTrayWnd) {
                if(!CreateTrayIcon(g_hTrayWnd)) {
                    LogToFile("[WARN] Cannot create tray icon");
                }
            }

            wchar_t exeDir[MAX_PATH];
            GetModuleFileNameW(NULL, exeDir, MAX_PATH);
            wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
            if(lastSlash) *(lastSlash + 1) = L'\0';

            Config_Init(exeDir);
            if (!Config_Load()) {
                LogToFile("[Config] No config found, scanning directory...");
                ScanPlaylist(exeDir);
                Config_Save();
            }

            // FIX/NEW: nếu config.ini có CurrentWallpaper hợp lệ (do Manager
            // ghi lúc Apply/double-click, hoặc do chính Engine ghi lần chạy
            // trước — xem LoadMediaByIndex), ưu tiên path này hơn
            // CurrentTrack index — match theo đường dẫn thật trong
            // g_playlist thay vì tin tưởng chỉ số mảng, vì index có thể
            // lệch giữa 2 app (xem giải thích trong config.h).
            if (g_currentWallpaperPath[0] != L'\0' &&
                GetFileAttributesW(g_currentWallpaperPath) != INVALID_FILE_ATTRIBUTES) {
                int foundIdx = -1;
                for (size_t i = 0; i < g_playlist.size(); i++) {
                    if (_wcsicmp(g_playlist[i].c_str(), g_currentWallpaperPath) == 0) {
                        foundIdx = (int)i;
                        break;
                    }
                }
                if (foundIdx < 0) {
                    // Không có trong playlist hiện tại (VD Manager quản lý
                    // playlist riêng, khác playlist Engine tự scan) — vẫn
                    // thêm vào để load được đúng file.
                    g_playlist.push_back(g_currentWallpaperPath);
                    foundIdx = (int)g_playlist.size() - 1;
                }
                g_currentTrack = foundIdx;
                LogToFile("[Config] CurrentWallpaper override -> track %d: %S",
                    g_currentTrack, g_currentWallpaperPath);
            }

            wchar_t mediaPath[MAX_PATH] = L"";
            if(argc >= 2) {
                wcscpy(mediaPath, argv[1]);
                if(GetFileAttributesW(mediaPath) != INVALID_FILE_ATTRIBUTES) {
                    g_playlist.clear();
                    g_playlist.push_back(mediaPath);
                    g_currentTrack = 0;
                }
            }

            if(argv) LocalFree(argv);

            if(!g_playlist.empty() && g_currentTrack >= 0) {
                LoadMediaByIndex(g_currentTrack);
            } else {
                LogToFile("[WARN] No media found, lightweight demo mode");
                UpdateTrayTooltip(L"Mate++ Lightweight");
            }

            LogToFile("--- Running Lightweight Mode ---");
            LogToFile("Memory optimized: no audio");

            PipeServer_Start();

            HANDLE hRenderThread = CreateThread(NULL, 0, RenderThread, NULL, 0, NULL);
            if(!hRenderThread) {
                LogToFile("[ERR] Failed to create render thread");
                Cleanup();
                return 1;
            }

            MSG msg;
            while(g_running && GetMessage(&msg, NULL, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            LogToFile("\n[..] Shutting down...");
            g_running = false;
            if(WaitForSingleObject(hRenderThread, 3000) == WAIT_TIMEOUT) {
                TerminateThread(hRenderThread, 0);
            }
            CloseHandle(hRenderThread);
            Cleanup();
            LogToFile("[OK] Done!");

            return 0;
        } catch(...) {
            LogToFile("[EXCEPTION] FATAL EXCEPTION in WinMain");
            InterlockedIncrement(&g_exceptionCount);
            MessageBoxW(NULL,
                L"Wallpaper Engine crashed with a fatal error!\n"
                L"Check wallpaper_log.txt for details.",
                L"Mate++ - Fatal Error",
                MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        LogToFile("[EXCEPTION] Recovered from fatal error via longjmp");
        MessageBoxW(NULL,
            L"Wallpaper Engine recovered from a fatal error!\n"
            L"Check wallpaper_log.txt for details.",
            L"Mate++ - Recovered",
            MB_OK | MB_ICONWARNING);
        return 1;
    }
}
