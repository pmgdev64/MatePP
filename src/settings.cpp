// settings.cpp - Settings UI for Mate++ Wallpaper Engine
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "common.h"
#include "settings.h"

#pragma comment(lib, "comctl32")

// ============================================================
//  Resource IDs
// ============================================================
#define IDD_SIMPLE_SETTINGS     100
#define IDC_VOLUME_SLIDER       1002
#define IDC_VOLUME_LABEL        1003
#define IDC_LOOP_CHECK          1004
#define IDC_TOP_MOST_CHECK      1005
#define IDC_PLAY_PAUSE_BTN      1006
#define IDC_NEXT_BTN            1007
#define IDC_PREV_BTN            1008
#define IDC_RELOAD_BTN          1009
#define IDC_MUTE_BTN            1012

// ============================================================
//  Global Settings Dialog Instance
// ============================================================
static HWND g_hSettingsDlg = NULL;

// ============================================================
//  Settings Dialog Procedure
// ============================================================
static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LogToFile("[Settings] Dialog initialized");

            // Center dialog
            RECT rc;
            GetWindowRect(hDlg, &rc);
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            int x = (screenWidth - (rc.right - rc.left)) / 2;
            int y = (screenHeight - (rc.bottom - rc.top)) / 2;
            SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

            // Initialize volume slider
            SendDlgItemMessage(hDlg, IDC_VOLUME_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendDlgItemMessage(hDlg, IDC_VOLUME_SLIDER, TBM_SETPOS, TRUE, (LPARAM)(g_volume * 100));

            // Update volume label
            wchar_t volText[16];
            swprintf_s(volText, L"%.0f%%", g_volume * 100);
            SetDlgItemText(hDlg, IDC_VOLUME_LABEL, volText);

            // Set checkboxes
            CheckDlgButton(hDlg, IDC_LOOP_CHECK, g_isLooping ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_TOP_MOST_CHECK, g_isTopMost ? BST_CHECKED : BST_UNCHECKED);

            // Set play/pause button
            SetDlgItemText(hDlg, IDC_PLAY_PAUSE_BTN, g_isPaused ? L"Play" : L"Pause");

            // Set mute button
            SetDlgItemText(hDlg, IDC_MUTE_BTN, g_isMuted ? L"🔇 Unmute" : L"🔊 Mute");

            // Store dialog handle
            g_hSettingsDlg = hDlg;

            return TRUE;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDC_PLAY_PAUSE_BTN: {
                    g_isPaused = !g_isPaused;
                    SetDlgItemText(hDlg, IDC_PLAY_PAUSE_BTN, g_isPaused ? L"Play" : L"Pause");
                    LogToFile("[Settings] %s", g_isPaused ? "Paused" : "Resumed");
                    return TRUE;
                }

                case IDC_NEXT_BTN: {
                    LogToFile("[Settings] Next track");
                    LoadNextMedia();
                    return TRUE;
                }

                case IDC_PREV_BTN: {
                    LogToFile("[Settings] Previous track");
                    LoadPrevMedia();
                    return TRUE;
                }

                case IDC_RELOAD_BTN: {
                    LogToFile("[Settings] Reload media");
                    ReloadCurrentMedia();
                    return TRUE;
                }

                case IDC_MUTE_BTN: {
                    g_isMuted = !g_isMuted;
                    SetDlgItemText(hDlg, IDC_MUTE_BTN, g_isMuted ? L"🔇 Unmute" : L"🔊 Mute");
                    LogToFile("[Settings] %s", g_isMuted ? "Muted" : "Unmuted");
                    return TRUE;
                }

                case IDC_LOOP_CHECK: {
                    g_isLooping = (IsDlgButtonChecked(hDlg, IDC_LOOP_CHECK) == BST_CHECKED);
                    LogToFile("[Settings] Loop: %s", g_isLooping ? "ON" : "OFF");
                    return TRUE;
                }

                case IDC_TOP_MOST_CHECK: {
                    g_isTopMost = (IsDlgButtonChecked(hDlg, IDC_TOP_MOST_CHECK) == BST_CHECKED);

                    // Update main wallpaper window
                    HWND hWorker = FindWindowW(L"MatePPWallpaperClass", NULL);
                    if (hWorker) {
                        SetWindowPos(hWorker, g_isTopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                    }
                    LogToFile("[Settings] Always on Top: %s", g_isTopMost ? "ON" : "OFF");
                    return TRUE;
                }

                case IDCANCEL:
                case IDOK: {
                    LogToFile("[Settings] Dialog closed");
                    g_hSettingsDlg = NULL;
                    EndDialog(hDlg, LOWORD(wParam));
                    return TRUE;
                }
            }
            break;
        }

        case WM_HSCROLL: {
            if ((HWND)lParam == GetDlgItem(hDlg, IDC_VOLUME_SLIDER)) {
                int pos = (int)SendDlgItemMessage(hDlg, IDC_VOLUME_SLIDER, TBM_GETPOS, 0, 0);
                g_volume = pos / 100.0f;

                wchar_t volText[16];
                swprintf_s(volText, L"%.0f%%", g_volume * 100);
                SetDlgItemText(hDlg, IDC_VOLUME_LABEL, volText);

                LogToFile("[Settings] Volume set to %.0f%%", g_volume * 100);
            }
            return TRUE;
        }

        case WM_CLOSE: {
            LogToFile("[Settings] Dialog closed via X button");
            g_hSettingsDlg = NULL;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
    }

    return FALSE;
}

// ============================================================
//  Public Functions
// ============================================================
void ShowSettingsDialog(HWND parent) {
    try {
        // If dialog already open, bring it to front
        if (g_hSettingsDlg && IsWindow(g_hSettingsDlg)) {
            SetForegroundWindow(g_hSettingsDlg);
            ShowWindow(g_hSettingsDlg, SW_SHOW);
            LogToFile("[Settings] Dialog already open, brought to front");
            return;
        }

        // Create dialog from resource
        HINSTANCE hInst = GetModuleHandle(NULL);
        INT_PTR result = DialogBox(hInst, MAKEINTRESOURCE(IDD_SIMPLE_SETTINGS), parent, SettingsDlgProc);

        if (result == -1) {
            LogToFile("[Settings] Failed to create dialog! Error: %d", GetLastError());
        }
    } catch (...) {
        LogToFile("[EXCEPTION] ShowSettingsDialog");
        g_hSettingsDlg = NULL;
    }
}

void CloseSettingsDialog() {
    if (g_hSettingsDlg && IsWindow(g_hSettingsDlg)) {
        SendMessage(g_hSettingsDlg, WM_CLOSE, 0, 0);
        g_hSettingsDlg = NULL;
    }
}

bool IsSettingsDialogOpen() {
    return g_hSettingsDlg != NULL && IsWindow(g_hSettingsDlg);
}

void UpdateSettingsUI() {
    if (g_hSettingsDlg && IsWindow(g_hSettingsDlg)) {
        // Update play/pause button
        SetDlgItemText(g_hSettingsDlg, IDC_PLAY_PAUSE_BTN, g_isPaused ? L"Play" : L"Pause");

        // Update volume label
        wchar_t volText[16];
        swprintf_s(volText, L"%.0f%%", g_volume * 100);
        SetDlgItemText(g_hSettingsDlg, IDC_VOLUME_LABEL, volText);

        // Update volume slider
        SendDlgItemMessage(g_hSettingsDlg, IDC_VOLUME_SLIDER, TBM_SETPOS, TRUE, (LPARAM)(g_volume * 100));

        // Update mute button
        SetDlgItemText(g_hSettingsDlg, IDC_MUTE_BTN, g_isMuted ? L"🔇 Unmute" : L"🔊 Mute");

        // Update checkboxes
        CheckDlgButton(g_hSettingsDlg, IDC_LOOP_CHECK, g_isLooping ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDlg, IDC_TOP_MOST_CHECK, g_isTopMost ? BST_CHECKED : BST_UNCHECKED);
    }
}
