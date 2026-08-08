// config.cpp - Lưu/đọc playlist + settings (NO AUDIO)
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <mutex>

#include "config.h"
#include "common.h"

wchar_t g_configPath[MAX_PATH] = L"";
wchar_t g_currentWallpaperPath[MAX_PATH] = L"";

// ============================================================
//  Init
// ============================================================
void Config_Init(const wchar_t* exeDir) {
    wcscpy_s(g_configPath, exeDir);
    wcscat_s(g_configPath, L"config.ini");
    LogToFile("[Config] Path: %S", g_configPath);
}

// ============================================================
//  Load
// ============================================================
bool Config_Load() {
    if (!g_configPath[0]) return false;

    if (GetFileAttributesW(g_configPath) == INVALID_FILE_ATTRIBUTES) {
        LogToFile("[Config] config.ini not found");
        return false;
    }

    // ---- Settings ----
    wchar_t buf[64];
    // BỎ VOLUME
    // GetPrivateProfileStringW(L"Settings", L"Volume", L"1.0", buf, 64, g_configPath);
    // g_volume = (float)_wtof(buf);

    g_isLooping = GetPrivateProfileIntW(L"Settings", L"Loop", 1, g_configPath) != 0;
    // g_isMuted   = GetPrivateProfileIntW(L"Settings", L"Muted", 0, g_configPath) != 0;  // BỎ
    g_isTopMost = GetPrivateProfileIntW(L"Settings", L"TopMost", 0, g_configPath) != 0;
    int savedTrack = GetPrivateProfileIntW(L"Settings", L"CurrentTrack", 0, g_configPath);

    // FIX/NEW: đọc path wallpaper hiện tại được lưu trực tiếp (không qua
    // index) — Manager (mppmgr.exe) ghi key này mỗi khi người dùng bấm
    // Apply / double-click 1 item, tức là thời điểm wallpaper THẬT SỰ đổi.
    GetPrivateProfileStringW(L"Settings", L"CurrentWallpaper", L"",
        g_currentWallpaperPath, MAX_PATH, g_configPath);

    // ---- Playlist ----
    int count = GetPrivateProfileIntW(L"Playlist", L"Count", 0, g_configPath);
    {
        std::lock_guard<std::mutex> lk(g_playlistMtx);
        g_playlist.clear();

        wchar_t key[16];
        wchar_t path[MAX_PATH];
        for (int i = 0; i < count; i++) {
            swprintf_s(key, L"%d", i);
            GetPrivateProfileStringW(L"Playlist", key, L"", path, MAX_PATH, g_configPath);
            if (path[0] == L'\0') continue;

            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
                LogToFile("[Config] Skip missing file: %S", path);
                continue;
            }
            g_playlist.push_back(path);
        }

        if (!g_playlist.empty()) {
            g_currentTrack = (savedTrack >= 0 && savedTrack < (int)g_playlist.size()) ? savedTrack : 0;
        } else {
            g_currentTrack = -1;
        }
    }

    LogToFile("[Config] Loaded: %zu tracks, current=%d, loop=%d, topmost=%d",
              g_playlist.size(), g_currentTrack, g_isLooping, g_isTopMost);

    return !g_playlist.empty();
}

// ============================================================
//  Save
// ============================================================
void Config_Save() {
    if (!g_configPath[0]) return;

    // ---- Settings ----
    wchar_t buf[64];
    // BỎ VOLUME
    // swprintf_s(buf, L"%.3f", g_volume);
    // WritePrivateProfileStringW(L"Settings", L"Volume", buf, g_configPath);
    WritePrivateProfileStringW(L"Settings", L"Loop", g_isLooping ? L"1" : L"0", g_configPath);
    // WritePrivateProfileStringW(L"Settings", L"Muted", g_isMuted ? L"1" : L"0", g_configPath);  // BỎ
    WritePrivateProfileStringW(L"Settings", L"TopMost", g_isTopMost ? L"1" : L"0", g_configPath);
    swprintf_s(buf, L"%d", g_currentTrack);
    WritePrivateProfileStringW(L"Settings", L"CurrentTrack", buf, g_configPath);

    // FIX/NEW: giữ CurrentWallpaper luôn khớp thực tế — nếu không ghi lại
    // ở đây, mỗi lần Engine tự đổi track (Next/Prev/pipe LOAD) mà không
    // cập nhật key này, lần khởi động sau Engine sẽ đọc lại giá trị CŨ
    // do Manager ghi từ trước, load nhầm file không còn đang hiển thị.
    if (g_currentWallpaperPath[0] != L'\0') {
        WritePrivateProfileStringW(L"Settings", L"CurrentWallpaper", g_currentWallpaperPath, g_configPath);
    }

    // ---- Playlist ----
    wchar_t emptySection[2] = { 0, 0 };
    WritePrivateProfileSectionW(L"Playlist", emptySection, g_configPath);

    std::lock_guard<std::mutex> lk(g_playlistMtx);
    swprintf_s(buf, L"%zu", g_playlist.size());
    WritePrivateProfileStringW(L"Playlist", L"Count", buf, g_configPath);

    wchar_t key[16];
    for (size_t i = 0; i < g_playlist.size(); i++) {
        swprintf_s(key, L"%zu", i);
        WritePrivateProfileStringW(L"Playlist", key, g_playlist[i].c_str(), g_configPath);
    }

    LogToFile("[Config] Saved (%zu tracks)", g_playlist.size());
}
