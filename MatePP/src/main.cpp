// main.cpp - FFMPEG VERSION
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

#include "common.h"
#include "settings.h"
#include "about.h"
#include "pipe_server.h"

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
#define ID_TRAY_VOLUME_UP    1008
#define ID_TRAY_VOLUME_DOWN  1009
#define ID_TRAY_MUTE         1010
#define ID_TRAY_SETTINGS     1011
#define ID_TRAY_ABOUT        1012
#define ID_TRAY_LOOP         1013
#define ID_TRAY_TOP_MOST     1014
#define ID_TRAY_PERFORMANCE   1015
#define ID_TRAY_DEBUG_TOGGLE  1016

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

// ============================================================
template<typename T> inline void SafeRelease(T*& p) {
    if(p) {
        p->Release();
        p = NULL;
    }
}

// ============================================================
//  Globals - DEFINITIONS (shared with video_decoder.cpp)
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

// CPU pixel double-buffer
BYTE* g_pixBuf[2]        = {NULL, NULL};
int   g_pixBack          = 0;
std::mutex   g_pixMtx;
volatile bool g_newFrameReady = false;
HANDLE g_hFrameConsumed = NULL;

// Timing anchor
LONGLONG g_vidStartWall = 0;
LONGLONG g_vidStartTS   = 0;
LARGE_INTEGER g_qpfFreq = {0};

// System Tray globals
NOTIFYICONDATAW g_nid = {0};
bool g_trayIconCreated = false;
HWND g_hMainWnd = NULL;
HWND g_hTrayWnd = NULL;

// Media control globals
bool g_isPaused = false;
float g_volume = 1.0f;
bool g_isMuted = false;
bool g_isLooping = true;
bool g_isTopMost = false;
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
//  Signal Handlers (for crashes)
// ============================================================
static void SignalHandler(int signal) {
    const char* sigName = "UNKNOWN";
    switch(signal) {
        case SIGSEGV: sigName = "SIGSEGV (Access Violation)"; break;
        case SIGABRT: sigName = "SIGABRT (Abort)"; break;
        case SIGFPE:  sigName = "SIGFPE (Floating Point Exception)"; break;
        case SIGILL:  sigName = "SIGILL (Illegal Instruction)"; break;
        case SIGTERM: sigName = "SIGTERM (Termination)"; break;
        case SIGINT:  sigName = "SIGINT (Interrupt)"; break;
    }

    LogToFile("[SIGNAL] Caught signal: %s (%d)", sigName, signal);
    InterlockedIncrement(&g_exceptionCount);

    FILE* f = fopen("crash_report.txt", "w");
    if(f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "=== Wallpaper Engine Crash Report ===\n");
        fprintf(f, "Date: %04d-%02d-%02d %02d:%02d:%02d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "Signal: %s (%d)\n", sigName, signal);
        void* stack[64];
        USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);
        fprintf(f, "Call Stack:\n");
        for(USHORT i = 0; i < frames; i++) {
            fprintf(f, "  #%d: 0x%p\n", i, stack[i]);
        }
        fclose(f);
    }

    if(g_hasJumpBuffer) {
        longjmp(g_jumpBuffer, 1);
    } else {
        exit(1);
    }
}

static void InitExceptionHandlers() {
    signal(SIGSEGV, SignalHandler);
    signal(SIGABRT, SignalHandler);
    signal(SIGFPE, SignalHandler);
    signal(SIGILL, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGINT, SignalHandler);
    LogToFile("[EXCEPTION] Signal handlers registered");
}

// ============================================================
//  GIF Player
// ============================================================
static struct GifPlayer {
    std::vector<ID2D1Bitmap*> frames;
    std::vector<UINT> delays;
    int cur=0;
    DWORD last=0;
    UINT w=0,h=0;
    bool loaded=false;
    std::mutex mtx;

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

            for(UINT i = 0; i < fc; i++) {
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
    for(int i = 0; i < 15; i++) {
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
    wcscpy_s(g_nid.szTip, L"Wallpaper Engine D2D");

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
//  LoadMediaByIndex
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
            wchar_t tip[128];
            const wchar_t* filename = wcsrchr(path.c_str(), L'\\');
            if (g_useVideo && g_vidLoaded) {
                swprintf_s(tip, L"▶ %s", filename ? filename + 1 : path.c_str());
            } else if (g_gif.loaded) {
                swprintf_s(tip, L"▶ %s", filename ? filename + 1 : path.c_str());
            } else {
                wcscpy_s(tip, L"Wallpaper Engine D2D");
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

// ============================================================
//  Reload Media
// ============================================================
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
            std::lock_guard<std::mutex> renderLock(g_renderMtx);
            SafeRelease(g_pVideoBmp);
            free(g_pixBuf[0]); g_pixBuf[0] = NULL;
            free(g_pixBuf[1]); g_pixBuf[1] = NULL;
            g_gif.Release();
            g_vidLoaded = false;
            g_useVideo  = false;
            g_isPaused  = false;
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
                } else if (g_gif.loaded) {
                    swprintf_s(tip, L"▶ %s", filename ? filename + 1 : path.c_str());
                } else {
                    wcscpy_s(tip, L"Wallpaper Engine D2D");
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
//  Context Menu
// ============================================================
static void ShowContextMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"Show Wallpaper");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_HIDE, L"Hide Wallpaper");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    HMENU hMediaMenu = CreatePopupMenu();
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_PLAY_PAUSE,
                g_isPaused ? L"Play" : L"Pause");
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_NEXT, L"Next Track");
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_PREV, L"Previous Track");
    AppendMenuW(hMediaMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_VOLUME_UP, L"Volume +");
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_VOLUME_DOWN, L"Volume -");
    AppendMenuW(hMediaMenu, MF_STRING, ID_TRAY_MUTE,
                g_isMuted ? L"Unmute" : L"Mute");
    AppendMenuW(hMediaMenu, MF_SEPARATOR, 0, NULL);
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
            if (g_hWorker && IsWindow(g_hWorker)) {
                ShowWindow(g_hWorker, SW_SHOW);
                UpdateTrayTooltip(L"Wallpaper Engine D2D");
            }
            break;

        case ID_TRAY_HIDE:
            if (g_hWorker && IsWindow(g_hWorker)) {
                ShowWindow(g_hWorker, SW_HIDE);
                UpdateTrayTooltip(L"Wallpaper Engine (Hidden)");
            }
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

        case ID_TRAY_VOLUME_UP:
            g_volume = std::min(1.0f, g_volume + 0.1f);
            LogToFile("[Media] Volume: %.0f%%", g_volume * 100);
            break;

        case ID_TRAY_VOLUME_DOWN:
            g_volume = std::max(0.0f, g_volume - 0.1f);
            LogToFile("[Media] Volume: %.0f%%", g_volume * 100);
            break;

        case ID_TRAY_MUTE:
            g_isMuted = !g_isMuted;
            LogToFile("[Media] %s", g_isMuted ? "Muted" : "Unmuted");
            UpdateTrayTooltip(g_isMuted ? L"Muted" : L"Unmuted");
            break;

        case ID_TRAY_LOOP:
            g_isLooping = !g_isLooping;
            LogToFile("[Media] Loop: %s", g_isLooping ? "ON" : "OFF");
            break;

        case ID_TRAY_TOP_MOST:
            g_isTopMost = !g_isTopMost;
            LogToFile("[Display] Always on Top: %s", g_isTopMost ? "ON" : "OFF");
            if (g_hWorker && IsWindow(g_hWorker)) {
                SetWindowPos(g_hWorker, g_isTopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
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
                if (g_hWorker && IsWindow(g_hWorker)) {
                    if (IsWindowVisible(g_hWorker)) {
                        ShowWindow(g_hWorker, SW_HIDE);
                        UpdateTrayTooltip(L"Wallpaper Engine (Hidden)");
                    } else {
                        ShowWindow(g_hWorker, SW_SHOW);
                        UpdateTrayTooltip(L"Wallpaper Engine D2D");
                    }
                }
                break;

            case WM_RBUTTONDOWN:
                ShowContextMenu(g_hMainWnd);
                break;

            case WM_LBUTTONDOWN:
                if (g_hWorker && IsWindow(g_hWorker)) {
                    bool visible = IsWindowVisible(g_hWorker);
                    ShowWindow(g_hWorker, visible ? SW_HIDE : SW_SHOW);
                    UpdateTrayTooltip(visible ? L"Wallpaper Engine (Hidden)" : L"Wallpaper Engine D2D");
                }
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
        wc.lpszClassName = L"WallpaperEngineTrayClass";

        if (!RegisterClassExW(&wc)) {
            LogToFile("[ERR] Failed to register window class");
            return NULL;
        }

        HWND hWnd = CreateWindowExW(
            0,
            L"WallpaperEngineTrayClass",
            L"Wallpaper Engine Tray",
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
//  Init Functions
// ============================================================
static HWND GetWorkerW() {
    try {
        HWND hP = FindWindow(L"Progman", NULL);
        if(!hP) return NULL;
        SendMessageTimeout(hP, 0x052C, 0, 0, SMTO_NORMAL, 1000, NULL);
        HWND hW = NULL;
        EnumWindows([](HWND h, LPARAM l)->BOOL{
            if(FindWindowEx(h, NULL, L"SHELLDLL_DefView", NULL)) {
                *(HWND*)l = FindWindowEx(NULL, h, L"WorkerW", NULL);
                return FALSE;
            }
            return TRUE;
        }, (LPARAM)&hW);
        return hW ? hW : hP;
    } catch(...) {
        LogToFile("[EXCEPTION] GetWorkerW");
        InterlockedIncrement(&g_exceptionCount);
        return NULL;
    }
}

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
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwp = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(sw, sh));

        hr = g_pD2DFactory->CreateHwndRenderTarget(rtp, hwp, &g_pRT);
        if(FAILED(hr)) {
            LogToFile("[ERR] RT: 0x%08X", (unsigned)hr);
            return false;
        }

        g_pRT->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &g_pBrush);
        g_pRT->CreateSolidColorBrush(D2D1::ColorF(0, 1, 0, 1), &g_pDebugBrush);
        LogToFile("[OK] D2D ready (%dx%d)", sw, sh);
        return true;
    } catch(...) {
        LogToFile("[EXCEPTION] InitD2D");
        InterlockedIncrement(&g_exceptionCount);
        return false;
    }
}

// ============================================================
//  Render
// ============================================================
static void RenderFrame() {
    try {
        if(!g_pRT) return;

        // Chỉ lock khi đọc frame mới, unlock ngay sau đó
        if(g_useVideo && g_vidLoaded) {
            ReadVideoFrame();
        }

        // Lock render resource vùng tối thiểu
        g_renderMtx.lock();

        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        if(!sw || !sh) {
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
        } else {
            float cx = sw * .5f, cy = sh * .5f;
            for(int i = 0; i < 6; i++) {
                float r = 80 + i * 70 + sinf(g_time * 1.2f - i * .4f) * 20;
                g_pBrush->SetColor(D2D1::ColorF(.3f, .6f, 1, .15f));
                g_pRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_pBrush, 2);
            }
            g_pBrush->SetColor(D2D1::ColorF(1, 1, 1, .6f));
            const wchar_t* m = L"No media - place wallpaper file next to exe";
            g_pRT->DrawText(m, (UINT)wcslen(m), g_pTextFmt,
                           D2D1::RectF(cx-400, cy-20, cx+400, cy+20), g_pBrush);
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

            wchar_t debugStr[512];
            swprintf_s(debugStr,
                L"FPS: %.1f\nCPU: %.1f%%\nRes: %dx%d\nVideo: %s\nPaused: %s\nLoop: %s\nFrameDur: %dms",
                g_fps, cpuUsage,
                g_useVideo ? g_vidW : g_gif.w, g_useVideo ? g_vidH : g_gif.h,
                g_vidLoaded ? (g_useVideo ? L"Video" : L"GIF") : L"None",
                g_isPaused ? L"Yes" : L"No",
                g_isLooping ? L"ON" : L"OFF",
                g_frameDur);

            D2D1_RECT_F bgRect = D2D1::RectF(8.0f, 8.0f, 300.0f, 160.0f);
            g_pDebugBrush->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));
            g_pRT->FillRectangle(bgRect, g_pDebugBrush);
            g_pDebugBrush->SetColor(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f));
            g_pRT->DrawText(debugStr, (UINT)wcslen(debugStr), g_pDebugTextFmt,
                           D2D1::RectF(12.0f, 12.0f, 300.0f, 160.0f), g_pDebugBrush);
        }

        HRESULT hr = g_pRT->EndDraw();
        g_renderMtx.unlock();

        if(FAILED(hr)) {
            LogToFile("[Render] EndDraw failed: 0x%08X", (unsigned)hr);
        }
    } catch(...) {
        LogToFile("[EXCEPTION] RenderFrame");
        InterlockedIncrement(&g_exceptionCount);
        g_renderMtx.unlock();
    }
}

// Render thread - UNLOCKED
// Trong main.cpp, sửa RenderThread:
static DWORD WINAPI RenderThread(LPVOID) {
    try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        LARGE_INTEGER f, p, n;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&p);

        while(g_running) {
            QueryPerformanceCounter(&n);
            g_time += (float)((double)(n.QuadPart - p.QuadPart) / f.QuadPart);
            p = n;

            RenderFrame();

            // NO SLEEP - chạy max speed, GPU tự lo
        }

        return 0;
    } catch(...) {
        LogToFile("[EXCEPTION] RenderThread");
        InterlockedIncrement(&g_exceptionCount);
        return 1;
    }
}
// ============================================================
//  Cleanup
// ============================================================
static void Cleanup() {
    try {
        PipeServer_Stop();
        LogToFile("[..] Cleaning up...");

        CloseSettingsDialog();

        // Dừng decode thread TRƯỚC
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
        g_renderMtx.unlock();

        // Đóng event handles SAU KHI thread đã dừng
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
//  WinMain - Entry Point
// ============================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    InitExceptionHandlers();

    g_hasJumpBuffer = true;
    if(setjmp(g_jumpBuffer) == 0) {
        try {
            LogToFile("===================================================");
            LogToFile("Mate++ Wallpaper Engine D2D v2.0 (FFmpeg) Starting...");
            LogToFile("Exception Handling: C++ try/catch + Signal Handlers");
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

            ScanPlaylist(exeDir);

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
                LogToFile("[WARN] No media found, demo mode");
                UpdateTrayTooltip(L"Wallpaper Engine (Demo Mode)");
            }

            LogToFile("--- Running 60fps MT ---");
            LogToFile("System tray icon available (right-click for menu)");
            LogToFile("Exception Handling: Active");

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
                L"Wallpaper Engine - Fatal Error",
                MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        LogToFile("[EXCEPTION] Recovered from fatal error via longjmp");
        MessageBoxW(NULL,
            L"Wallpaper Engine recovered from a fatal error!\n"
            L"Check wallpaper_log.txt for details.",
            L"Wallpaper Engine - Recovered",
            MB_OK | MB_ICONWARNING);
        return 1;
    }
}
