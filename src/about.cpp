// about.cpp - About dialog for Mate++ Wallpaper Engine
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include "about.h"
#include "common.h"

// ============================================================
//  Resource ID
// ============================================================
#define IDD_ABOUT    101
#define IDI_MAIN_ICON 101

// ============================================================
//  Dialog Proc
// ============================================================
INT_PTR CALLBACK AboutDialog::DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Center on screen
            RECT rc;
            GetWindowRect(hDlg, &rc);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
            int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
            SetWindowPos(hDlg, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);

            // Show exception count in title bar
            wchar_t title[128];
            swprintf_s(title, L"About Mate++  [exceptions: %d]", g_exceptionCount);
            SetWindowTextW(hDlg, title);

            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL || LOWORD(wParam) == IDOK) {
                EndDialog(hDlg, LOWORD(wParam));
                return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

// ============================================================
//  Public
// ============================================================
void AboutDialog::Show(HWND parent) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    INT_PTR res = DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUT), parent, DlgProc);
    if (res == -1) {
        // Fallback nếu resource không load được
        wchar_t msg[512];
        swprintf_s(msg,
            L"Mate++ - Live Wallpaper Engine\n"
            L"Version 1.0.0\n\n"
            L"Author: PmgTeam (PepperMCGamers / PmgDev64)\n"
            L"Built with: Direct2D + Media Foundation + MinGW64\n"
            L"Supports: GIF, MP4, AVI, WMV, MKV, MOV, WEBM\n\n"
            L"License: Free to Use & Open-Source\n"
            L"Exceptions caught: %d",
            g_exceptionCount
        );
        MessageBoxW(parent, msg, L"About Mate++", MB_OK | MB_ICONINFORMATION);
        LogToFile("[About] Dialog resource failed (err %d), used MessageBox fallback", GetLastError());
    }
}
