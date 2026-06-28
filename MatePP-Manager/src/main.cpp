// mpp_hub.cpp - Mate++ Wallpaper Hub
// App riêng, giao tiếp với mpp.exe qua Named Pipe
// Drag & drop file, list có thumbnail nhỏ
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
#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

#pragma comment(lib, "comctl32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "user32")
#pragma comment(lib, "comdlg32")

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

#define THUMB_W         72
#define THUMB_H         54
#define ROW_H           64

#define WM_PIPE_STATUS  (WM_USER + 1)
#define WM_PIPE_ACK     (WM_USER + 2)
#define WM_ADD_ITEM     (WM_USER + 3)   // lParam = heap WallItem*, main thread frees

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

static HIMAGELIST g_hImgList = NULL;
static IWICImagingFactory* g_pWIC = NULL;

static std::atomic<bool>  g_pipeConnected{false};
static std::atomic<bool>  g_pipeRunning{false};
static HANDLE             g_hPipeThread = NULL;

struct WallItem {
    std::wstring path;
    std::wstring name;
    int          imgIdx;
    HBITMAP      hThumb = NULL;  // temp, used during WM_ADD_ITEM
};
static std::vector<WallItem> g_items;
static std::mutex            g_itemsMtx;

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
//  IPC — Named Pipe client
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

    // Send command
    DWORD written = 0, len = (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t));
    WriteFile(hPipe, cmd, len, &written, NULL);

    // Read reply (optional, non-blocking wait 500ms)
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

// Probe thread — kiểm tra mpp còn sống không mỗi 3s
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
//  Thumbnail — WIC decode to HBITMAP 72x54
// ============================================================
static HBITMAP MakeThumb(const wchar_t* path) {
    if (!g_pWIC) return NULL;

    IWICBitmapDecoder*     pDec  = NULL;
    IWICBitmapFrameDecode* pFrm  = NULL;
    IWICBitmapScaler*      pScl  = NULL;
    IWICFormatConverter*   pConv = NULL;
    HBITMAP hBmp = NULL;

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

// ============================================================
//  ListView helpers
// ============================================================
static void ListRebuild() {
    ListView_DeleteAllItems(g_hList);
    std::lock_guard<std::mutex> lk(g_itemsMtx);
    for (int i = 0; i < (int)g_items.size(); i++) {
        LVITEMW lvi = {};
        lvi.mask    = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem   = i;
        lvi.pszText = (LPWSTR)g_items[i].name.c_str();
        lvi.iImage  = g_items[i].imgIdx;
        lvi.lParam  = i;
        ListView_InsertItem(g_hList, &lvi);
    }
}

static void AddFile(const wchar_t* path) {
    // Extension check
    const wchar_t* ext = PathFindExtensionW(path);
    static const wchar_t* exts[] = {
        L".mp4", L".avi", L".wmv", L".mkv", L".mov",
        L".webm", L".m4v", L".gif", NULL
    };
    bool ok = false;
    for (int i = 0; exts[i]; i++)
        if (_wcsicmp(ext, exts[i]) == 0) { ok = true; break; }
    if (!ok) return;

    // Duplicate check
    {
        std::lock_guard<std::mutex> lk(g_itemsMtx);
        for (auto& it : g_items)
            if (_wcsicmp(it.path.c_str(), path) == 0) return;
    }

    // Make thumbnail on this thread (can be background)
    HBITMAP hBmp = MakeThumb(path);

    // Build item — ImageList_Add and ListView MUST happen on main thread
    WallItem* item = new WallItem();
    item->path   = path;
    item->name   = PathFindFileNameW(path);
    item->imgIdx = -1;
    item->hThumb = hBmp;   // main thread will call ImageList_Add

    // Post to main thread — WndProc handles WM_ADD_ITEM
    PostMessageW(g_hWnd, WM_ADD_ITEM, 0, (LPARAM)item);
    Log("Queued: %ws", path);
}

static int GetSelectedIndex() {
    return ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
}

// ============================================================
//  Preview panel — draw selected thumbnail large
// ============================================================
static void UpdatePreview(int idx) {
    if (!g_hPreview) return;
    InvalidateRect(g_hPreview, NULL, TRUE);
    // Store selected idx for WM_PAINT
    SetWindowLongPtr(g_hPreview, GWLP_USERDATA, (LONG_PTR)idx);
}

static LRESULT CALLBACK PreviewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        HBRUSH hBr = CreateSolidBrush(RGB(20, 20, 30));
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);

        int idx = (int)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        std::lock_guard<std::mutex> lk(g_itemsMtx);
        if (idx >= 0 && idx < (int)g_items.size() && g_items[idx].imgIdx >= 0) {
            // Draw thumbnail scaled up
            IMAGEINFO ii; ImageList_GetImageInfo(g_hImgList, g_items[idx].imgIdx, &ii);
            HDC hdcMem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(hdcMem, ii.hbmImage);
            int pw = rc.right - 8, ph = rc.bottom - 8;
            // Keep aspect
            int tw = THUMB_W, th = THUMB_H;
            float scale = std::min((float)pw/tw, (float)ph/th);
            int dw = (int)(tw * scale), dh = (int)(th * scale);
            int dx = (rc.right - dw) / 2, dy = (rc.bottom - dh) / 2;
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, dx, dy, dw, dh, hdcMem, 0, 0, tw, th, SRCCOPY);
            SelectObject(hdcMem, old);
            DeleteDC(hdcMem);
        } else {
            // No preview
            SetTextColor(hdc, RGB(80, 80, 100));
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(hdc, L"No preview", -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
//  Layout — called on WM_SIZE
// ============================================================
static void DoLayout(HWND hWnd) {
    RECT rc; GetClientRect(hWnd, &rc);
    int W = rc.right, H = rc.bottom;

    // Panels
    int leftW   = W * 55 / 100;
    int rightW  = W - leftW - 8;
    int btnH    = 28;
    int statusH = 22;
    int listH   = H - statusH - btnH - 16;
    int prevH   = listH - btnH - 8;

    // List
    SetWindowPos(g_hList, NULL,
        4, 4, leftW - 4, listH, SWP_NOZORDER);

    // Buttons left row
    int bw = (leftW - 12) / 3;
    SetWindowPos(g_hApply,  NULL, 4,            listH + 8, bw, btnH, SWP_NOZORDER);
    SetWindowPos(g_hRemove, NULL, 4 + bw + 4,   listH + 8, bw, btnH, SWP_NOZORDER);
    SetWindowPos(g_hClear,  NULL, 4 + bw*2 + 8, listH + 8, bw, btnH, SWP_NOZORDER);

    // Preview
    SetWindowPos(g_hPreview, NULL,
        leftW + 4, 4, rightW, prevH, SWP_NOZORDER);

    // Browse button
    SetWindowPos(g_hBrowse, NULL,
        leftW + 4, prevH + 8, rightW, btnH, SWP_NOZORDER);

    // Status bar
    SetWindowPos(g_hStatus, NULL,
        0, H - statusH, W, statusH, SWP_NOZORDER);
}

// ============================================================
//  Main Window Proc
// ============================================================
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        // ListView
        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hWnd, (HMENU)IDC_LIST, GetModuleHandle(NULL), NULL);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        // Columns
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx   = 60; col.pszText = (LPWSTR)L"";
        ListView_InsertColumn(g_hList, 0, &col);
        col.cx   = 380; col.pszText = (LPWSTR)L"File";
        ListView_InsertColumn(g_hList, 1, &col);

        // ImageList for thumbnails
        g_hImgList = ImageList_Create(THUMB_W, THUMB_H, ILC_COLOR32, 64, 16);
        ListView_SetImageList(g_hList, g_hImgList, LVSIL_SMALL);

        // Buttons
        g_hApply  = CreateWindowW(L"BUTTON", L"▶ Apply",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 0,0,0,0, hWnd, (HMENU)IDC_BTN_APPLY, NULL,NULL);
        g_hRemove = CreateWindowW(L"BUTTON", L"✕ Remove",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 0,0,0,0, hWnd, (HMENU)IDC_BTN_REMOVE, NULL,NULL);
        g_hClear  = CreateWindowW(L"BUTTON", L"🗑 Clear",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 0,0,0,0, hWnd, (HMENU)IDC_BTN_CLEAR, NULL,NULL);
        g_hBrowse = CreateWindowW(L"BUTTON", L"📂 Browse Files...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 0,0,0,0, hWnd, (HMENU)IDC_BTN_BROWSE, NULL,NULL);

        // Preview panel (custom class)
        WNDCLASSW wcp = {};
        wcp.lpfnWndProc   = PreviewWndProc;
        wcp.lpszClassName = L"HubPreview";
        wcp.hCursor       = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wcp);
        g_hPreview = CreateWindowExW(WS_EX_CLIENTEDGE, L"HubPreview", NULL,
            WS_CHILD|WS_VISIBLE, 0,0,0,0, hWnd, (HMENU)IDC_PREVIEW, NULL, NULL);
        SetWindowLongPtr(g_hPreview, GWLP_USERDATA, -1);

        // Status bar
        g_hStatus = CreateWindowW(STATUSCLASSNAMEW, L"mpp: not connected",
            WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP, 0,0,0,0,
            hWnd, (HMENU)IDC_STATUS, GetModuleHandle(NULL), NULL);

        // Accept drag & drop
        DragAcceptFiles(hWnd, TRUE);

        // Start pipe probe thread
        g_pipeRunning = true;
        g_hPipeThread = CreateThread(NULL, 0, PipeProbeThread, NULL, 0, NULL);

        return 0;
    }

    case WM_SIZE:
        DoLayout(hWnd);
        return 0;

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH];
            DragQueryFileW(hDrop, i, buf, MAX_PATH);
            // Run AddFile on a thread so UI stays responsive for large drops
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
                if (nmlv->uNewState & LVIS_SELECTED)
                    UpdatePreview(nmlv->iItem);
            }
            if (nm->code == NM_DBLCLK) {
                // Double click = apply immediately
                int sel = GetSelectedIndex();
                if (sel >= 0) {
                    std::lock_guard<std::mutex> lk(g_itemsMtx);
                    if (sel < (int)g_items.size()) {
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
                MessageBoxW(hWnd, L"Chọn một file trước.", L"Hub", MB_OK|MB_ICONINFORMATION);
                break;
            }
            std::wstring cmd;
            {
                std::lock_guard<std::mutex> lk(g_itemsMtx);
                if (sel < (int)g_items.size())
                    cmd = L"LOAD:" + g_items[sel].path;
            }
            if (!cmd.empty())
                std::thread([cmd]{ SendToPipe(cmd.c_str()); }).detach();
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
            ListRebuild();
            UpdatePreview(-1);
            break;
        }

        case IDC_BTN_CLEAR: {
            if (MessageBoxW(hWnd, L"Xóa toàn bộ playlist?", L"Hub",
                    MB_YESNO|MB_ICONQUESTION) == IDYES) {
                {
                    std::lock_guard<std::mutex> lk(g_itemsMtx);
                    g_items.clear();
                }
                ImageList_RemoveAll(g_hImgList);
                ListRebuild();
                UpdatePreview(-1);
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
                // Multi-select: directory + null-separated filenames
                wchar_t* p = files;
                wchar_t dir[MAX_PATH]; wcscpy_s(dir, p);
                p += wcslen(p) + 1;
                if (*p == L'\0') {
                    // Single file selected
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

        } // switch LOWORD
        return 0;
    }

    case WM_ADD_ITEM: {
        WallItem* item = (WallItem*)lParam;
        if (!item) return 0;
        // ImageList_Add on main thread
        if (item->hThumb) {
            item->imgIdx = ImageList_Add(g_hImgList, item->hThumb, NULL);
            DeleteObject(item->hThumb);
            item->hThumb = NULL;
        }
        // Append to list and insert into ListView directly (no full rebuild)
        int idx;
        {
            std::lock_guard<std::mutex> lk(g_itemsMtx);
            g_items.push_back(*item);
            idx = (int)g_items.size() - 1;
        }
        delete item;

        LVITEMW lvi = {};
        lvi.mask    = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem   = idx;
        lvi.pszText = (LPWSTR)g_items[idx].name.c_str();
        lvi.iImage  = g_items[idx].imgIdx;
        lvi.lParam  = idx;
        ListView_InsertItem(g_hList, &lvi);
        // Sub-item col1 = filename (col0 = thumbnail icon)
        ListView_SetItemText(g_hList, idx, 1, (LPWSTR)g_items[idx].name.c_str());
        return 0;
    }

    case WM_PIPE_STATUS: {
        bool conn = (wParam == 1);
        SetWindowTextW(g_hStatus,
            conn ? L"  ✔ mpp connected" : L"  ✖ mpp not connected — mở mpp.exe trước");
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

    // WIC factory
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory, (void**)&g_pWIC);

    // Init common controls
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WC_HUB;
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wc.hIconSm       = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        WC_HUB, L"Mate++ Manager",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 560,
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
