// common.h - Shared declarations between main.cpp and settings.cpp
#pragma once

#ifndef COMMON_H
#define COMMON_H

#include <windows.h>
#include <vector>
#include <string>
#include <mutex>

// ============================================================
//  Global Variables - Declared as extern
// ============================================================
extern bool g_isPaused;
extern bool g_isLooping;
extern bool g_isTopMost;
extern float g_volume;
extern bool g_isMuted;
extern int g_currentTrack;
extern std::vector<std::wstring> g_playlist;
extern std::mutex g_playlistMtx;
extern volatile LONG g_exceptionCount;
extern volatile bool g_isLoading;
extern std::mutex g_loadMtx;

// ============================================================
//  Global Functions - SỬA KIỂU TRẢ VỀ
// ============================================================
extern void LogToFile(const char* msg, ...);
extern void ReloadCurrentMedia();
extern bool LoadNextMedia();    // Sửa từ void thành bool
extern bool LoadPrevMedia();    // Sửa từ void thành bool
extern bool LoadMediaByIndex(int index);
extern void UpdateTrayTooltip(const wchar_t* tip);
extern void ScanPlaylist(const wchar_t* directory);

#endif // COMMON_H
