#include "eol_notice.h"
#include "notice_client.h"
#include <string>
#include <vector>

namespace EolNotice {

static const char* kClassName = "MatePPEolNoticeClass";  // ANSI
static const UINT WM_NOTICE_READY = WM_APP + 1;

// Dialog context
struct DialogContext {
    HWND hDlg = NULL;
    HWND hEdit = NULL;
    HWND hClose = NULL;
    HWND hRetry = NULL;
    HFONT hFont = NULL;
    bool closed = true;
    bool registered = false;
};

static DialogContext g_ctx;

// Convert string to CRLF
static std::wstring ToCRLF(const std::wstring& src) {
    std::wstring out;
    out.reserve(src.size() + 64);
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == L'\n' && (i == 0 || src[i-1] != L'\r')) {
            out += L'\r';
        }
        out += src[i];
    }
    return out;
}

// Create font with fallback
static HFONT CreateSafeFont() {
    HFONT font = CreateFontW(
        -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    if (!font) {
        font = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Courier New");
    }

    if (!font) {
        font = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_ROMAN, L"Tahoma");
    }

    return font;
}

// Clean string - remove invalid characters
static std::wstring CleanString(const std::wstring& str) {
    std::wstring result;
    result.reserve(str.size());
    for (wchar_t c : str) {
        if ((c >= 32 && c < 127) || c == L'\n' || c == L'\r' || c == L'\t') {
            result += c;
        }
    }
    return result;
}

struct FetchThreadParam {
    HWND hDlg;
};

static DWORD WINAPI FetchThread(LPVOID param) {
    FetchThreadParam* p = (FetchThreadParam*)param;
    HWND hDlg = p->hDlg;
    delete p;

    NoticeClient::EolResult* res = new NoticeClient::EolResult();
    *res = NoticeClient::FetchEolNotice();

    if (IsWindow(hDlg)) {
        PostMessageW(hDlg, WM_NOTICE_READY, 0, (LPARAM)res);
    } else {
        delete res;
    }
    return 0;
}

static void StartFetch(HWND hDlg) {
    if (g_ctx.hEdit && IsWindow(g_ctx.hEdit)) {
        SetWindowTextA(g_ctx.hEdit, "Loading notice from matepp.vercel.app...");  // ANSI
    }
    if (g_ctx.hRetry && IsWindow(g_ctx.hRetry)) {
        ShowWindow(g_ctx.hRetry, SW_HIDE);
    }

    FetchThreadParam* p = new FetchThreadParam{ hDlg };
    HANDLE hThread = CreateThread(NULL, 0, FetchThread, p, 0, NULL);

    if (!hThread) {
        delete p;
        if (g_ctx.hEdit && IsWindow(g_ctx.hEdit)) {
            SetWindowTextA(g_ctx.hEdit, "Cannot load notice. Please check network connection.");  // ANSI
        }
        if (g_ctx.hRetry && IsWindow(g_ctx.hRetry)) {
            ShowWindow(g_ctx.hRetry, SW_SHOW);
        }
    } else {
        CloseHandle(hThread);
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_ctx.hFont = CreateSafeFont();

            g_ctx.hEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                0, 0, 0, 0, hWnd, (HMENU)101,
                GetModuleHandle(NULL), NULL);

            if (g_ctx.hEdit && g_ctx.hFont) {
                SendMessageW(g_ctx.hEdit, WM_SETFONT, (WPARAM)g_ctx.hFont, TRUE);
            }

            g_ctx.hRetry = CreateWindowExW(
                0, L"BUTTON", L"Retry",
                WS_CHILD | BS_PUSHBUTTON,
                0, 0, 100, 30, hWnd, (HMENU)103,
                GetModuleHandle(NULL), NULL);

            if (g_ctx.hRetry && g_ctx.hFont) {
                SendMessageW(g_ctx.hRetry, WM_SETFONT, (WPARAM)g_ctx.hFont, TRUE);
            }
            ShowWindow(g_ctx.hRetry, SW_HIDE);

            g_ctx.hClose = CreateWindowExW(
                0, L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                0, 0, 100, 30, hWnd, (HMENU)102,
                GetModuleHandle(NULL), NULL);

            if (g_ctx.hClose && g_ctx.hFont) {
                SendMessageW(g_ctx.hClose, WM_SETFONT, (WPARAM)g_ctx.hFont, TRUE);
            }

            SetFocus(g_ctx.hClose);
            StartFetch(hWnd);
            break;
        }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hWnd, &rc);
            int margin = 10;
            int btnH = 30;
            int btnW = 100;
            int spacing = 8;

            if (g_ctx.hEdit && IsWindow(g_ctx.hEdit)) {
                MoveWindow(g_ctx.hEdit, margin, margin,
                    rc.right - margin * 2,
                    rc.bottom - margin * 3 - btnH, TRUE);
            }

            if (g_ctx.hRetry && IsWindow(g_ctx.hRetry)) {
                MoveWindow(g_ctx.hRetry,
                    rc.right - margin - btnW * 2 - spacing,
                    rc.bottom - margin - btnH, btnW, btnH, TRUE);
            }

            if (g_ctx.hClose && IsWindow(g_ctx.hClose)) {
                MoveWindow(g_ctx.hClose,
                    rc.right - margin - btnW,
                    rc.bottom - margin - btnH, btnW, btnH, TRUE);
            }
            break;
        }

        case WM_NOTICE_READY: {
            NoticeClient::EolResult* res = (NoticeClient::EolResult*)lParam;

            if (res) {
                if (res->ok) {
                    // FIX: Dùng SetWindowTextA với ASCII thuần
                    SetWindowTextA(hWnd, "Mate++ Wallpaper Engine - Notice");  // ANSI

                    // Set body
                    if (g_ctx.hEdit && IsWindow(g_ctx.hEdit)) {
                        std::wstring body = ToCRLF(res->body);
                        body = CleanString(body);
                        SetWindowTextW(g_ctx.hEdit, body.c_str());
                    }

                    if (g_ctx.hRetry && IsWindow(g_ctx.hRetry)) {
                        ShowWindow(g_ctx.hRetry, SW_HIDE);
                    }

                } else {
                    std::wstring msg = L"Cannot load notice.\r\n\r\n";
                    msg += L"Error: " + CleanString(res->error);
                    msg += L"\r\n\r\nPlease check network connection and try again.";

                    if (g_ctx.hEdit && IsWindow(g_ctx.hEdit)) {
                        SetWindowTextW(g_ctx.hEdit, msg.c_str());
                    }

                    if (g_ctx.hRetry && IsWindow(g_ctx.hRetry)) {
                        ShowWindow(g_ctx.hRetry, SW_SHOW);
                    }
                }

                delete res;
            }
            break;
        }

        case WM_COMMAND: {
            int cmd = LOWORD(wParam);
            if (cmd == 102) { // Close
                DestroyWindow(hWnd);
            } else if (cmd == 103) { // Retry
                StartFetch(hWnd);
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hWnd);
            break;

        case WM_DESTROY:
            if (g_ctx.hFont) {
                DeleteObject(g_ctx.hFont);
                g_ctx.hFont = NULL;
            }
            g_ctx.closed = true;
            g_ctx.hDlg = NULL;
            break;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void Show(HWND parent) {
    HINSTANCE hInst = GetModuleHandle(NULL);

    if (!g_ctx.registered) {
        WNDCLASSEXA wc = {0};  // ANSI version
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(101));
        wc.lpszClassName = kClassName;
        wc.lpszMenuName = NULL;

        if (RegisterClassExA(&wc)) {  // ANSI
            g_ctx.registered = true;
        }
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 720;
    int h = 620;
    int x = (sw - w) / 2;
    int y = (sh - h) / 2;

    if (parent && IsWindow(parent)) {
        EnableWindow(parent, FALSE);
    }

    // Tạo window với ANSI title
    g_ctx.hDlg = CreateWindowExA(  // ANSI version
        WS_EX_DLGMODALFRAME,
        kClassName,
        "Mate++ Wallpaper Engine - Notice",  // ANSI string - KHÔNG có L
        (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
        x, y, w, h, parent, NULL, hInst, NULL);

    if (!g_ctx.hDlg) {
        if (parent && IsWindow(parent)) {
            EnableWindow(parent, TRUE);
        }
        return;
    }

    g_ctx.closed = false;

    MSG msg;
    while (!g_ctx.closed && GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            if (msg.hwnd == g_ctx.hDlg || IsChild(g_ctx.hDlg, msg.hwnd)) {
                DestroyWindow(g_ctx.hDlg);
                continue;
            }
        }

        if (!IsDialogMessage(g_ctx.hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (parent && IsWindow(parent)) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }
}

} // namespace EolNotice
