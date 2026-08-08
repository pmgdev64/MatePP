// main.cpp - Mate++ Wallpaper Hub - FIXED
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <winerror.h>
#include <wincodec.h>
#include <uxtheme.h>
#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

// FFmpeg headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "comctl32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "uxtheme")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "user32")
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")

// ============================================================
//  Constants
// ============================================================
#define WC_HUB          L"MatePPHubClass"
#define PIPE_NAME       L"\\\\.\\pipe\\MatePPHub"
#define PIPE_TIMEOUT    2000

#define IDI_MAIN_ICON   101
#define IDC_LIST        1001
#define IDC_BTN_APPLY   1002
#define IDC_BTN_REMOVE  1003
#define IDC_BTN_CLEAR   1004
#define IDC_BTN_BROWSE  1005
#define IDC_STATUS      1006
#define IDC_PREVIEW     1007
#define IDC_BTN_UP      1008
#define IDC_BTN_DOWN    1009

#define THUMB_W         80
#define THUMB_H         60
#define ROW_H           68

// Định nghĩa LVM_SETITEMHEIGHT nếu chưa có
#ifndef LVM_SETITEMHEIGHT
#define LVM_SETITEMHEIGHT (LVM_FIRST + 141)
#endif

#define WM_PIPE_STATUS  (WM_USER + 1)
#define WM_ADD_ITEM     (WM_USER + 3)

// ============================================================
//  Colors
// ============================================================
#define COLOR_BG       RGB(32, 32, 40)
#define COLOR_PANEL    RGB(40, 40, 50)
#define COLOR_BORDER   RGB(60, 60, 75)
#define COLOR_TEXT     RGB(220, 220, 230)
#define COLOR_TEXT_DIM RGB(140, 140, 160)
#define COLOR_ACCENT   RGB(70, 130, 255)
#define COLOR_BTN_HOVER RGB(70, 70, 90)
#define COLOR_LIST_BG  RGB(25, 25, 32)

// ============================================================
//  Globals
// ============================================================
static HWND  g_hWnd      = NULL;
static HWND  g_hList     = NULL;
static HWND  g_hStatus   = NULL;
static HWND  g_hPreview  = NULL;
static HWND  g_hApply    = NULL;
static HWND  g_hRemove   = NULL;
static HWND  g_hClear    = NULL;
static HWND  g_hBrowse   = NULL;
static HWND  g_hBtnUp    = NULL;
static HWND  g_hBtnDown  = NULL;
static HWND  g_hTitle    = NULL;

static HIMAGELIST g_hImgList = NULL;
static IWICImagingFactory* g_pWIC = NULL;
static HFONT g_hTitleFont = NULL;
static HFONT g_hFont = NULL;

static std::atomic<bool>  g_pipeConnected{false};
static std::atomic<bool>  g_pipeRunning{false};
static HANDLE             g_hPipeThread = NULL;

static wchar_t      g_configPath[MAX_PATH] = L"";

struct WallItem {
    std::wstring path;
    std::wstring name;
    int          imgIdx;
    HBITMAP      hThumb = NULL;
};
static std::vector<WallItem> g_items;
static std::mutex            g_itemsMtx;

static std::wstring g_pendingSelectPath;
static std::mutex   g_pendingSelectMtx;
static std::atomic<bool> g_pendingSelectDone{false};

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
static void AddFile(const wchar_t* path);
static void SaveConfig();
static void UpdatePreview(int idx);
static HBITMAP ExtractVideoThumbnail(const wchar_t* path);

// ============================================================
//  Logging
// ============================================================
static void Log(const char* fmt, ...) {
    char buf[512];
    va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    OutputDebugStringA("[HUB] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ============================================================
//  Config functions
// ============================================================
static void InitConfigPath() {
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    wcscpy_s(g_configPath, exeDir);
    wcscat_s(g_configPath, L"config.ini");
    Log("Config path: %ws", g_configPath);
}

static void SaveConfig() {
    if (!g_configPath[0]) return;

    wchar_t emptySection[2] = { 0, 0 };
    WritePrivateProfileSectionW(L"Playlist", emptySection, g_configPath);

    wchar_t buf[64];
    swprintf_s(buf, L"%zu", g_items.size());
    WritePrivateProfileStringW(L"Playlist", L"Count", buf, g_configPath);

    wchar_t key[16];
    std::lock_guard<std::mutex> lk(g_itemsMtx);
    for (size_t i = 0; i < g_items.size(); i++) {
        swprintf_s(key, L"%zu", i);
        WritePrivateProfileStringW(L"Playlist", key, g_items[i].path.c_str(), g_configPath);
    }

    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < (int)g_items.size()) {
        swprintf_s(buf, L"%d", sel);
        WritePrivateProfileStringW(L"Settings", L"CurrentTrack", buf, g_configPath);
    }
}

static void LoadConfigPlaylist() {
    if (GetFileAttributesW(g_configPath) == INVALID_FILE_ATTRIBUTES) {
        Log("config.ini not found");
        return;
    }

    int count = GetPrivateProfileIntW(L"Playlist", L"Count", 0, g_configPath);
    int currentTrack = GetPrivateProfileIntW(L"Settings", L"CurrentTrack", -1, g_configPath);

    std::vector<std::wstring> paths;
    wchar_t key[16], path[MAX_PATH];
    for (int i = 0; i < count; i++) {
        swprintf_s(key, L"%d", i);
        GetPrivateProfileStringW(L"Playlist", key, L"", path, MAX_PATH, g_configPath);
        if (path[0] == L'\0') continue;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
        paths.push_back(path);
    }

    if (paths.empty()) return;

    if (currentTrack >= 0 && currentTrack < (int)paths.size()) {
        std::lock_guard<std::mutex> lk(g_pendingSelectMtx);
        g_pendingSelectPath = paths[currentTrack];
    }

    for (auto& p : paths) {
        AddFile(p.c_str());
    }
    g_pendingSelectDone = false;
}

// ============================================================
//  Extract video thumbnail using FFmpeg
// ============================================================
static HBITMAP ExtractVideoThumbnail(const wchar_t* path) {
    HBITMAP hBmp = NULL;
    AVFormatContext* pFmtCtx = NULL;
    AVCodecContext* pCodecCtx = NULL;
    AVFrame* pFrame = NULL;
    AVFrame* pFrameRGB = NULL;
    AVPacket* pPacket = NULL;
    SwsContext* pSwsCtx = NULL;
    uint8_t* buffer = NULL;

    char path_utf8[MAX_PATH * 4] = {0};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path_utf8, sizeof(path_utf8) - 1, NULL, NULL);

    do {
        if (avformat_open_input(&pFmtCtx, path_utf8, NULL, NULL) < 0) break;
        if (avformat_find_stream_info(pFmtCtx, NULL) < 0) break;

        int videoStream = -1;
        for (unsigned int i = 0; i < pFmtCtx->nb_streams; i++) {
            if (pFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStream = i;
                break;
            }
        }
        if (videoStream == -1) break;

        AVCodecParameters* codecpar = pFmtCtx->streams[videoStream]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) break;

        pCodecCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(pCodecCtx, codecpar);
        if (avcodec_open2(pCodecCtx, codec, NULL) < 0) break;

        pFrame = av_frame_alloc();
        pFrameRGB = av_frame_alloc();
        pPacket = av_packet_alloc();
        if (!pFrame || !pFrameRGB || !pPacket) break;

        int frameFound = 0;
        while (av_read_frame(pFmtCtx, pPacket) >= 0) {
            if (pPacket->stream_index == videoStream) {
                if (avcodec_send_packet(pCodecCtx, pPacket) >= 0) {
                    if (avcodec_receive_frame(pCodecCtx, pFrame) == 0) {
                        frameFound = 1;
                        break;
                    }
                }
            }
            av_packet_unref(pPacket);
        }

        if (!frameFound) break;

        int width = THUMB_W;
        int height = THUMB_H;
        int srcW = pCodecCtx->width;
        int srcH = pCodecCtx->height;

        float aspect = (float)srcW / srcH;
        if (aspect > 1.0f) {
            width = THUMB_W;
            height = (int)(THUMB_W / aspect);
        } else {
            height = THUMB_H;
            width = (int)(THUMB_H * aspect);
        }
        if (width < 1) width = 1;
        if (height < 1) height = 1;

        pSwsCtx = sws_getContext(srcW, srcH, (AVPixelFormat)pFrame->format,
                                  width, height, AV_PIX_FMT_BGRA,
                                  SWS_BILINEAR, NULL, NULL, NULL);
        if (!pSwsCtx) break;

        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_BGRA, width, height, 1);
        buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));

        av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer,
                             AV_PIX_FMT_BGRA, width, height, 1);

        sws_scale(pSwsCtx, pFrame->data, pFrame->linesize, 0, srcH,
                  pFrameRGB->data, pFrameRGB->linesize);

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pBits = NULL;
        HDC hdc = GetDC(NULL);
        hBmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ReleaseDC(NULL, hdc);

        if (hBmp && pBits) {
            memcpy(pBits, pFrameRGB->data[0], width * height * 4);
        }

    } while (0);

    if (buffer) av_free(buffer);
    if (pSwsCtx) sws_freeContext(pSwsCtx);
    if (pPacket) av_packet_free(&pPacket);
    if (pFrameRGB) av_frame_free(&pFrameRGB);
    if (pFrame) av_frame_free(&pFrame);
    if (pCodecCtx) avcodec_free_context(&pCodecCtx);
    if (pFmtCtx) avformat_close_input(&pFmtCtx);

    return hBmp;
}

// FIX: đường link mới giữa Manager và Engine (mpp.exe), độc lập với
// việc 2 exe có nằm chung thư mục hay không và không cần pipe đang
// sống — xem giải thích đầy đủ ở config.h/config.cpp phía project
// Engine. Key/value ở đây PHẢI khớp chính xác với những gì
// Config_LoadCurrentFromManager() bên Engine đọc.
// FIX/NEW: ghi key CurrentWallpaper vào config.ini — nguồn duy nhất Engine
// đọc để biết wallpaper nào đang được áp dụng. Phải khớp đúng section/key
// mà Config_Load() bên Engine (config.cpp) đọc: [Settings] CurrentWallpaper.
// Chỉ gọi tại đúng thời điểm wallpaper THẬT SỰ đổi (Apply / double-click),
// không đặt trong SaveConfig() chung vì hàm đó bị gọi cả khi chỉ đổi
// selection trong list, chưa chắc đã áp dụng.
static void SaveCurrentWallpaperToConfig(const std::wstring& path) {
    if (!g_configPath[0]) return;
    WritePrivateProfileStringW(L"Settings", L"CurrentWallpaper", path.c_str(), g_configPath);
}

// ============================================================
//  IPC
// ============================================================
static bool SendToPipe(const wchar_t* cmd) {
    HANDLE hPipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        g_pipeConnected = false;
        PostMessage(g_hWnd, WM_PIPE_STATUS, 0, 0);
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    DWORD written = 0, len = (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t));
    WriteFile(hPipe, cmd, len, &written, NULL);

    char reply[512] = {};
    DWORD read = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ReadFile(hPipe, reply, sizeof(reply) - 1, &read, &ov);
    WaitForSingleObject(ov.hEvent, 500);
    CloseHandle(ov.hEvent);
    CloseHandle(hPipe);

    g_pipeConnected = true;
    PostMessage(g_hWnd, WM_PIPE_STATUS, 1, 0);
    return true;
}

static DWORD WINAPI PipeProbeThread(LPVOID) {
    while (g_pipeRunning) {
        HANDLE hPipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, 0, NULL);
        bool ok = (hPipe != INVALID_HANDLE_VALUE);
        if (ok) CloseHandle(hPipe);
        if (ok != g_pipeConnected.load()) {
            g_pipeConnected = ok;
            PostMessage(g_hWnd, WM_PIPE_STATUS, ok ? 1 : 0, 0);
        }
        Sleep(3000);
    }
    return 0;
}

// ============================================================
//  Thumbnail
// ============================================================
static HBITMAP MakeThumb(const wchar_t* path) {
    HBITMAP hBmp = ExtractVideoThumbnail(path);
    if (hBmp) return hBmp;

    if (!g_pWIC) return NULL;

    IWICBitmapDecoder*     pDec  = NULL;
    IWICBitmapFrameDecode* pFrm  = NULL;
    IWICBitmapScaler*      pScl  = NULL;
    IWICFormatConverter*   pConv = NULL;
    hBmp = NULL;

    do {
        if (FAILED(g_pWIC->CreateDecoderFromFilename(path, NULL,
                GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDec))) break;
        if (FAILED(pDec->GetFrame(0, &pFrm))) break;
        if (FAILED(g_pWIC->CreateBitmapScaler(&pScl))) break;
        if (FAILED(pScl->Initialize(pFrm, THUMB_W, THUMB_H,
                WICBitmapInterpolationModeFant))) break;
        if (FAILED(g_pWIC->CreateFormatConverter(&pConv))) break;
        if (FAILED(pConv->Initialize(pScl, GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, NULL, 0.0,
                WICBitmapPaletteTypeCustom))) break;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = THUMB_W;
        bi.bmiHeader.biHeight      = -(int)THUMB_H;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pBits = NULL;
        HDC hdc = GetDC(NULL);
        hBmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ReleaseDC(NULL, hdc);
        if (!hBmp || !pBits) break;

        UINT stride = THUMB_W * 4;
        UINT bufSz  = stride * THUMB_H;
        pConv->CopyPixels(NULL, stride, bufSz, (BYTE*)pBits);
    } while (0);

    if (pConv) pConv->Release();
    if (pScl)  pScl->Release();
    if (pFrm)  pFrm->Release();
    if (pDec)  pDec->Release();

    return hBmp;
}

// FIX: LVS_REPORT row height in a Win32 ListView is authoritatively driven
// by the height of the small-icon ImageList attached to it — NOT reliably
// by the undocumented LVM_SETITEMHEIGHT message, whose effect turned out
// to differ between themed and non-themed (SetWindowTheme("","")) draw
// paths. That's exactly why disabling the Explorer theme (previous fix)
// made the row's background/selection box stop matching the thumbnail's
// height: once untethered from the theme, the ListView fell back to
// sizing rows from the ImageList's actual height (THUMB_H = 60) plus
// default system padding, which no longer agreed with ROW_H.
// Fix: make the ImageList itself ROW_H tall, and composite every
// thumbnail onto a ROW_H-tall canvas (centered, padded with the list's
// background color) before it's added — so the row height the ListView
// derives is always exactly what's on screen, independent of theme state.
static HBITMAP PadThumbToRowHeight(HBITMAP hSrc) {
    if (!hSrc) return NULL;

    BITMAP bm = {};
    GetObject(hSrc, sizeof(bm), &bm);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = THUMB_W;
    bi.bmiHeader.biHeight      = -ROW_H;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HDC hdcScreen = GetDC(NULL);
    HBITMAP hDst = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hDst || !pBits) {
        ReleaseDC(NULL, hdcScreen);
        return hSrc; // fall back to the unpadded bitmap rather than lose it
    }

    HDC hdcDst = CreateCompatibleDC(hdcScreen);
    HGDIOBJ oldDst = SelectObject(hdcDst, hDst);

    HBRUSH hBrBg = CreateSolidBrush(COLOR_LIST_BG);
    RECT rcAll = {0, 0, THUMB_W, ROW_H};
    FillRect(hdcDst, &rcAll, hBrBg);
    DeleteObject(hBrBg);

    HDC hdcSrc = CreateCompatibleDC(hdcScreen);
    HGDIOBJ oldSrc = SelectObject(hdcSrc, hSrc);

    int dx = (THUMB_W - bm.bmWidth) / 2;
    int dy = (ROW_H - bm.bmHeight) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    BitBlt(hdcDst, dx, dy, bm.bmWidth, bm.bmHeight, hdcSrc, 0, 0, SRCCOPY);

    SelectObject(hdcSrc, oldSrc);
    DeleteDC(hdcSrc);
    SelectObject(hdcDst, oldDst);
    DeleteDC(hdcDst);
    ReleaseDC(NULL, hdcScreen);

    DeleteObject(hSrc);
    return hDst;
}

// ============================================================
//  ListView helpers
// ============================================================
// FIX: previously the name-column width was computed by guessing a fixed
// scrollbar allowance (GetSystemMetrics(SM_CXVSCROLL) + 8) regardless of
// whether a scrollbar was actually visible. When no scrollbar was showing,
// that guess left an un-highlighted gap on the right edge of full-row-select
// (visible as the selection box stopping short of the real border).
// GetClientRect() on the ListView itself already excludes any currently
// visible scrollbar and the WS_EX_CLIENTEDGE border (both are non-client
// area), so measuring it directly is both simpler and always correct —
// no manual guessing needed, and it self-adjusts the moment a scrollbar
// appears or disappears.
static void ResizeListColumn() {
    if (!g_hList) return;
    RECT rc;
    GetClientRect(g_hList, &rc);
    int col0Width = THUMB_W + 10;
    int colWidth = rc.right - col0Width;
    if (colWidth < 60) colWidth = 60;
    ListView_SetColumnWidth(g_hList, 1, colWidth);
}

static void ListRebuild() {
    ListView_DeleteAllItems(g_hList);
    std::lock_guard<std::mutex> lk(g_itemsMtx);
    for (int i = 0; i < (int)g_items.size(); i++) {
        LVITEMW lvi = {};
        // FIX: column 0 only holds the thumbnail (90px wide, 80px used by
        // the image itself) — leave its item text empty so it doesn't
        // bleed a clipped glyph into column 1. The visible name is set
        // below via ListView_SetItemText on subitem 1.
        lvi.mask    = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem   = i;
        lvi.pszText = (LPWSTR)L"";
        lvi.iImage  = g_items[i].imgIdx;
        lvi.lParam  = i;
        ListView_InsertItem(g_hList, &lvi);
        ListView_SetItemText(g_hList, i, 1, (LPWSTR)g_items[i].name.c_str());
    }
    ResizeListColumn();
}

// FIX: AddFile() previously spawned a detached thread that captured the
// raw `path` pointer by value. That pointer only points into the CALLER's
// buffer (a temporary std::wstring in WM_DROPFILES / Browse / config load
// threads). Once the caller's thread finished and its wstring was
// destroyed, the detached worker thread here was still reading freed
// memory to build item->name / item->path -> garbled filenames, corrupted
// duplicate-check, and phantom empty rows in the ListView.
// Fix: copy the path into an owned std::wstring BEFORE spawning the
// thread, and capture that by value instead of the raw pointer.
static void AddFile(const wchar_t* path) {
    const wchar_t* ext = PathFindExtensionW(path);
    static const wchar_t* exts[] = {
        L".mp4", L".avi", L".wmv", L".mkv", L".mov",
        L".webm", L".m4v", L".gif", NULL
    };
    bool ok = false;
    for (int i = 0; exts[i]; i++)
        if (_wcsicmp(ext, exts[i]) == 0) { ok = true; break; }
    if (!ok) return;

    {
        std::lock_guard<std::mutex> lk(g_itemsMtx);
        for (auto& it : g_items)
            if (_wcsicmp(it.path.c_str(), path) == 0) return;
    }

    // Own a stable copy of the path before crossing thread boundaries.
    std::wstring pathCopy(path);

    std::thread([pathCopy]() {
        HBITMAP hBmp = MakeThumb(pathCopy.c_str());
        hBmp = PadThumbToRowHeight(hBmp);

        WallItem* item = new WallItem();
        item->path   = pathCopy;
        item->name   = PathFindFileNameW(pathCopy.c_str());
        item->imgIdx = -1;
        item->hThumb = hBmp;

        PostMessageW(g_hWnd, WM_ADD_ITEM, 0, (LPARAM)item);
        Log("Added: %ws", pathCopy.c_str());
    }).detach();
}

static int GetSelectedIndex() {
    return ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
}

// FIX: previously held g_itemsMtx via lock_guard for the whole function
// AND called ListRebuild(), which also locks g_itemsMtx internally.
// std::mutex is not recursive -> guaranteed deadlock the first time
// Up/Down was pressed. Fix: release the lock before calling ListRebuild().
static void MoveItem(int from, int to) {
    if (from < 0 || to < 0) return;

    {
        std::lock_guard<std::mutex> lk(g_itemsMtx);
        if (from >= (int)g_items.size() || to >= (int)g_items.size()) return;
        if (from == to) return;

        WallItem item = g_items[from];
        g_items.erase(g_items.begin() + from);
        g_items.insert(g_items.begin() + to, item);
    } // <-- lock released here, before ListRebuild() locks it again

    ListRebuild();
    ListView_SetItemState(g_hList, to, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_hList, to, FALSE);
    UpdatePreview(to);
    SaveConfig();
}

// ============================================================
//  Preview panel
// ============================================================
static void UpdatePreview(int idx) {
    if (!g_hPreview) return;
    InvalidateRect(g_hPreview, NULL, TRUE);
    SetWindowLongPtr(g_hPreview, GWLP_USERDATA, (LONG_PTR)idx);
}

static LRESULT CALLBACK PreviewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);

        HBRUSH hBrBg = CreateSolidBrush(COLOR_PANEL);
        FillRect(hdc, &rc, hBrBg);
        DeleteObject(hBrBg);

        HPEN hPen = CreatePen(PS_SOLID, 1, COLOR_BORDER);
        HPEN oldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, 0, 0, rc.right, rc.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(hPen);

        int idx = (int)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        std::lock_guard<std::mutex> lk(g_itemsMtx);

        if (idx >= 0 && idx < (int)g_items.size() && g_items[idx].imgIdx >= 0) {
            IMAGEINFO ii; ImageList_GetImageInfo(g_hImgList, g_items[idx].imgIdx, &ii);
            HDC hdcMem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(hdcMem, ii.hbmImage);

            int margin = 20;
            int pw = rc.right - margin * 2;
            int ph = rc.bottom - margin * 2 - 30;
            // FIX: read the actual per-frame rect instead of assuming
            // THUMB_W x THUMB_H at source offset (0,0) — ImageList packs
            // every frame into one strip bitmap, so a hardcoded (0,0)
            // offset only ever showed frame 0 correctly; and frame size
            // is now ROW_H tall (padded), not THUMB_H.
            int srcX = ii.rcImage.left;
            int srcY = ii.rcImage.top;
            int tw = ii.rcImage.right - ii.rcImage.left;
            int th = ii.rcImage.bottom - ii.rcImage.top;

            float scale = std::min((float)pw/tw, (float)ph/th);
            int dw = (int)(tw * scale), dh = (int)(th * scale);
            int dx = (rc.right - dw) / 2;
            int dy = (rc.bottom - 30 - dh) / 2;

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, dx, dy, dw, dh, hdcMem, srcX, srcY, tw, th, SRCCOPY);

            SelectObject(hdcMem, old);
            DeleteDC(hdcMem);

            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_TEXT);
            RECT txtRc = {margin, rc.bottom - 28, rc.right - margin, rc.bottom - 6};

            std::wstring displayName = g_items[idx].name;
            size_t dotPos = displayName.find_last_of(L'.');
            if (dotPos != std::wstring::npos) {
                displayName = displayName.substr(0, dotPos);
            }

            DrawTextW(hdc, displayName.c_str(), -1, &txtRc,
                DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, hOldFont);
        } else {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_TEXT_DIM);
            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFont);
            DrawTextW(hdc, L"No selection", -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hOldFont);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
//  Layout
// ============================================================
static void DoLayout(HWND hWnd) {
    RECT rc; GetClientRect(hWnd, &rc);
    int W = rc.right, H = rc.bottom;

    int titleH = 42;
    int leftW = W * 55 / 100;
    int rightW = W - leftW - 10;
    int btnH = 28;
    int statusH = 22;
    int margin = 6;

    int listTop = titleH + margin;
    int listBottom = H - statusH - btnH - margin * 2 - 4;
    int listH = listBottom - listTop;
    int prevTop = titleH + margin;
    int prevBottom = H - statusH - btnH - margin * 2 - 4;
    int prevH = prevBottom - prevTop;

    SetWindowPos(g_hTitle, NULL, 0, 0, W, titleH, SWP_NOZORDER);
    SetWindowPos(g_hList, NULL, margin, listTop, leftW - margin, listH, SWP_NOZORDER);

    int totalBtnW = leftW - margin - 4;
    int bw = (totalBtnW - 8) / 4;
    int btnY = listBottom + margin;
    SetWindowPos(g_hApply,  NULL, margin,            btnY, bw, btnH, SWP_NOZORDER);
    SetWindowPos(g_hRemove, NULL, margin + bw + 4,   btnY, bw, btnH, SWP_NOZORDER);
    SetWindowPos(g_hClear,  NULL, margin + bw*2 + 8, btnY, bw, btnH, SWP_NOZORDER);
    SetWindowPos(g_hBrowse, NULL, margin + bw*3 + 12,btnY, bw, btnH, SWP_NOZORDER);

    SetWindowPos(g_hPreview, NULL, leftW + margin, prevTop, rightW, prevH, SWP_NOZORDER);

    int btnW2 = (rightW - 4) / 2;
    int btnY2 = prevBottom + margin;
    SetWindowPos(g_hBtnUp, NULL, leftW + margin, btnY2, btnW2, btnH, SWP_NOZORDER);
    SetWindowPos(g_hBtnDown, NULL, leftW + margin + btnW2 + 4, btnY2, btnW2, btnH, SWP_NOZORDER);

    SetWindowPos(g_hStatus, NULL, 0, H - statusH, W, statusH, SWP_NOZORDER);

    // Set item height cho ListView - dùng LVM_SETITEMHEIGHT (đã định nghĩa)
    SendMessage(g_hList, LVM_SETITEMHEIGHT, ROW_H, 0);

    // FIX: dùng chung ResizeListColumn() — đo GetClientRect() thật của
    // ListView thay vì đoán chỗ chừa scrollbar bằng tay (số cũ luôn trừ
    // dù không có scrollbar, để lại dải highlight bị hụt mép phải).
    ResizeListColumn();
}

// ============================================================
//  Title bar
// ============================================================
static void DrawTitleBar(HWND hWnd, HDC hdc) {
    RECT rc; GetClientRect(hWnd, &rc);
    rc.bottom = 42;

    HBRUSH hBr = CreateSolidBrush(COLOR_BG);
    FillRect(hdc, &rc, hBr);
    DeleteObject(hBr);

    HPEN hPen = CreatePen(PS_SOLID, 1, COLOR_BORDER);
    HPEN oldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, 0, 41, NULL);
    LineTo(hdc, rc.right, 41);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);

    HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MAIN_ICON));
    if (hIcon) {
        DrawIconEx(hdc, 10, 6, hIcon, 28, 28, 0, NULL, DI_NORMAL);
        DestroyIcon(hIcon);
    }

    HFONT hOldFont = (HFONT)SelectObject(hdc, g_hTitleFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COLOR_TEXT);
    RECT txtRc = {46, 4, 250, 36};
    DrawTextW(hdc, L"Mate++ Manager", -1, &txtRc, DT_LEFT | DT_SINGLELINE);
    SelectObject(hdc, hOldFont);
}

static LRESULT CALLBACK TitleProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        DrawTitleBar(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        ReleaseCapture();
        SendMessage(GetParent(hWnd), WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
//  Main Window Proc
// ============================================================
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        avformat_network_init();

        g_hTitleFont = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_hFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        WNDCLASSW wct = {};
        wct.lpfnWndProc = TitleProc;
        wct.hInstance = GetModuleHandle(NULL);
        wct.lpszClassName = L"HubTitleBar";
        wct.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
        RegisterClassW(&wct);
        g_hTitle = CreateWindowW(L"HubTitleBar", NULL,
            WS_CHILD | WS_VISIBLE, 0,0,0,0, hWnd, NULL, NULL, NULL);

        // FIX: added LVS_NOCOLUMNHEADER — the columns have no header text
        // (pszText = L""), so the header row was rendering as a blank
        // white bar above the list (visible in the screenshot). Since we
        // never use the header for sorting/labels, just hide it.
        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
            0, 0, 0, 0, hWnd, (HMENU)IDC_LIST, GetModuleHandle(NULL), NULL);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        // FIX: under the Explorer visual style (default on comctl32 v6),
        // full-row-select draws a rounded "pill" highlight with its own
        // fixed internal padding, which does NOT match the row height set
        // via LVM_SETITEMHEIGHT below — visible as the blue selection
        // being shorter/taller than the actual row bounds. Turning off
        // the theme for just this control makes it draw a flat rectangle
        // that exactly matches the row height instead.
        SetWindowTheme(g_hList, L"", L"");

        ListView_SetBkColor(g_hList, COLOR_LIST_BG);
        ListView_SetTextBkColor(g_hList, COLOR_LIST_BG);
        ListView_SetTextColor(g_hList, COLOR_TEXT);

        // FIX: use Segoe UI (already created above) for the ListView
        // instead of the system default GUI font, and use a column width
        // that actually fits THUMB_W (80px) + padding instead of the old
        // 60px, which cropped every thumbnail on the right edge.
        SendMessage(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = THUMB_W + 10; col.pszText = (LPWSTR)L"";
        ListView_InsertColumn(g_hList, 0, &col);
        col.cx   = 350; col.pszText = (LPWSTR)L"";
        ListView_InsertColumn(g_hList, 1, &col);

        g_hImgList = ImageList_Create(THUMB_W, ROW_H, ILC_COLOR32, 64, 16);
        ListView_SetImageList(g_hList, g_hImgList, LVSIL_SMALL);

        // Set item height
        SendMessage(g_hList, LVM_SETITEMHEIGHT, ROW_H, 0);

        DWORD btnStyle = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
        g_hApply  = CreateWindowW(L"BUTTON", L"Apply", btnStyle, 0,0,0,0, hWnd, (HMENU)IDC_BTN_APPLY, NULL, NULL);
        g_hRemove = CreateWindowW(L"BUTTON", L"Remove", btnStyle, 0,0,0,0, hWnd, (HMENU)IDC_BTN_REMOVE, NULL, NULL);
        g_hClear  = CreateWindowW(L"BUTTON", L"Clear All", btnStyle, 0,0,0,0, hWnd, (HMENU)IDC_BTN_CLEAR, NULL, NULL);
        g_hBrowse = CreateWindowW(L"BUTTON", L"Browse...", btnStyle, 0,0,0,0, hWnd, (HMENU)IDC_BTN_BROWSE, NULL, NULL);
        g_hBtnUp  = CreateWindowW(L"BUTTON", L"Up", btnStyle, 0,0,0,0, hWnd, (HMENU)IDC_BTN_UP, NULL, NULL);
        g_hBtnDown= CreateWindowW(L"BUTTON", L"Down", btnStyle, 0,0,0,0, hWnd, (HMENU)IDC_BTN_DOWN, NULL, NULL);

        for (HWND btn : {g_hApply, g_hRemove, g_hClear, g_hBrowse, g_hBtnUp, g_hBtnDown}) {
            SendMessage(btn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }

        WNDCLASSW wcp = {};
        wcp.lpfnWndProc   = PreviewWndProc;
        wcp.lpszClassName = L"HubPreview";
        wcp.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wcp.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
        RegisterClassW(&wcp);
        g_hPreview = CreateWindowExW(WS_EX_CLIENTEDGE, L"HubPreview", NULL,
            WS_CHILD | WS_VISIBLE, 0,0,0,0, hWnd, (HMENU)IDC_PREVIEW, NULL, NULL);
        SetWindowLongPtr(g_hPreview, GWLP_USERDATA, -1);

        g_hStatus = CreateWindowW(STATUSCLASSNAMEW, L"mpp: not connected",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP | CCS_BOTTOM, 0,0,0,0,
            hWnd, (HMENU)IDC_STATUS, GetModuleHandle(NULL), NULL);
        SendMessage(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HBRUSH hBrBg = CreateSolidBrush(COLOR_BG);
        SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, (LONG_PTR)hBrBg);

        DragAcceptFiles(hWnd, TRUE);

        g_pipeRunning = true;
        g_hPipeThread = CreateThread(NULL, 0, PipeProbeThread, NULL, 0, NULL);

        std::thread(LoadConfigPlaylist).detach();

        return 0;
    }

    case WM_SIZE: {
        DoLayout(hWnd);
        InvalidateRect(g_hTitle, NULL, TRUE);
        return 0;
    }

    case WM_ERASEBKGND:
        return TRUE;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == g_hList) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_TEXT);
            SetBkColor(hdc, COLOR_LIST_BG);
            return (LRESULT)CreateSolidBrush(COLOR_LIST_BG);
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COLOR_TEXT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH];
            DragQueryFileW(hDrop, i, buf, MAX_PATH);
            std::wstring p(buf);
            std::thread([p]{ AddFile(p.c_str()); }).detach();
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lParam;
        if (nm->idFrom == IDC_LIST) {
            if (nm->code == LVN_ITEMCHANGED) {
                NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
                if (nmlv->uNewState & LVIS_SELECTED) {
                    UpdatePreview(nmlv->iItem);
                    SaveConfig();
                }
            }
            if (nm->code == NM_DBLCLK) {
                int sel = GetSelectedIndex();
                if (sel >= 0) {
                    std::lock_guard<std::mutex> lk(g_itemsMtx);
                    if (sel < (int)g_items.size()) {
                        SaveCurrentWallpaperToConfig(g_items[sel].path);
                        std::wstring cmd = L"LOAD:" + g_items[sel].path;
                        std::thread([cmd]{ SendToPipe(cmd.c_str()); }).detach();
                    }
                }
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {

        case IDC_BTN_APPLY: {
            int sel = GetSelectedIndex();
            if (sel < 0) {
                MessageBoxW(hWnd, L"Select a file first.", L"Hub", MB_OK|MB_ICONINFORMATION);
                break;
            }

            std::wstring path;
            {
                std::lock_guard<std::mutex> lk(g_itemsMtx);
                if (sel < (int)g_items.size())
                    path = g_items[sel].path;
            }

            if (!path.empty()) {
                SaveConfig();
                SaveCurrentWallpaperToConfig(path);
                std::wstring cmd = L"LOAD:" + path;
                std::thread([cmd]{ SendToPipe(cmd.c_str()); }).detach();
                Log("Applied: %ws", path.c_str());
            }
            break;
        }

        case IDC_BTN_REMOVE: {
            int sel = GetSelectedIndex();
            if (sel < 0) break;

            {
                std::lock_guard<std::mutex> lk(g_itemsMtx);
                if (sel < (int)g_items.size())
                    g_items.erase(g_items.begin() + sel);
            }

            ListView_DeleteItem(g_hList, sel);
            ResizeListColumn();
            UpdatePreview(-1);
            SaveConfig();

            std::thread([]{ SendToPipe(L"CLEAR"); }).detach();
            break;
        }

        case IDC_BTN_CLEAR: {
            if (MessageBoxW(hWnd, L"Clear entire playlist?", L"Hub",
                    MB_YESNO|MB_ICONQUESTION) == IDYES) {
                {
                    std::lock_guard<std::mutex> lk(g_itemsMtx);
                    g_items.clear();
                }
                ImageList_RemoveAll(g_hImgList);
                ListView_DeleteAllItems(g_hList);
                ResizeListColumn();
                UpdatePreview(-1);
                SaveConfig();
                std::thread([]{ SendToPipe(L"CLEAR"); }).detach();
            }
            break;
        }

        case IDC_BTN_BROWSE: {
            OPENFILENAMEW ofn = {};
            wchar_t files[4096] = {};
            ofn.lStructSize  = sizeof(ofn);
            ofn.hwndOwner    = hWnd;
            ofn.lpstrFilter  =
                L"Video/GIF\0*.mp4;*.avi;*.wmv;*.mkv;*.mov;*.webm;*.m4v;*.gif\0"
                L"All Files\0*.*\0";
            ofn.lpstrFile    = files;
            ofn.nMaxFile     = 4096;
            ofn.Flags        = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                wchar_t* p = files;
                wchar_t dir[MAX_PATH]; wcscpy_s(dir, p);
                p += wcslen(p) + 1;
                if (*p == L'\0') {
                    std::wstring path(dir);
                    std::thread([path]{ AddFile(path.c_str()); }).detach();
                } else {
                    while (*p) {
                        std::wstring path = std::wstring(dir) + L"\\" + p;
                        std::thread([path]{ AddFile(path.c_str()); }).detach();
                        p += wcslen(p) + 1;
                    }
                }
            }
            break;
        }

        case IDC_BTN_UP: {
            int sel = GetSelectedIndex();
            if (sel > 0) MoveItem(sel, sel - 1);
            break;
        }

        case IDC_BTN_DOWN: {
            int sel = GetSelectedIndex();
            if (sel >= 0 && sel < (int)g_items.size() - 1) MoveItem(sel, sel + 1);
            break;
        }

        } // switch
        return 0;
    }

    case WM_ADD_ITEM: {
        WallItem* item = (WallItem*)lParam;
        if (!item) return 0;

        if (item->hThumb) {
            item->imgIdx = ImageList_Add(g_hImgList, item->hThumb, NULL);
            DeleteObject(item->hThumb);
            item->hThumb = NULL;
        }

        int idx;
        {
            std::lock_guard<std::mutex> lk(g_itemsMtx);
            g_items.push_back(*item);
            idx = (int)g_items.size() - 1;
        }
        delete item;

        LVITEMW lvi = {};
        // FIX: see ListRebuild() — keep column 0's item text empty so it
        // doesn't clip-bleed a stray glyph next to the thumbnail.
        lvi.mask    = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem   = idx;
        lvi.pszText = (LPWSTR)L"";
        lvi.iImage  = g_items[idx].imgIdx;
        lvi.lParam  = idx;
        ListView_InsertItem(g_hList, &lvi);
        ListView_SetItemText(g_hList, idx, 1, (LPWSTR)g_items[idx].name.c_str());
        ResizeListColumn();

        if (!g_pendingSelectDone.load()) {
            bool match = false;
            {
                std::lock_guard<std::mutex> lk(g_pendingSelectMtx);
                if (!g_pendingSelectPath.empty() &&
                    _wcsicmp(g_pendingSelectPath.c_str(), g_items[idx].path.c_str()) == 0) {
                    match = true;
                }
            }
            if (match) {
                ListView_SetItemState(g_hList, idx,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(g_hList, idx, FALSE);
                UpdatePreview(idx);
                g_pendingSelectDone = true;
            }
        }

        SaveConfig();
        return 0;
    }

    case WM_PIPE_STATUS: {
        bool conn = (wParam == 1);
        SetWindowTextW(g_hStatus,
            conn ? L"mpp: connected" : L"mpp: not connected");
        return 0;
    }

    case WM_DESTROY:
        g_pipeRunning = false;
        if (g_hPipeThread) {
            WaitForSingleObject(g_hPipeThread, 1000);
            CloseHandle(g_hPipeThread);
        }
        if (g_pWIC) { g_pWIC->Release(); g_pWIC = NULL; }
        if (g_hImgList) { ImageList_Destroy(g_hImgList); }
        if (g_hTitleFont) DeleteObject(g_hTitleFont);
        if (g_hFont) DeleteObject(g_hFont);
        avformat_network_deinit();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================
//  WinMain
// ============================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory, (void**)&g_pWIC);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    InitConfigPath();

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WC_HUB;
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wc.hIconSm       = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        WC_HUB, L"Mate++ Manager",
        WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 500,
        NULL, NULL, hInst, NULL);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
