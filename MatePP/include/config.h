// config.h - Lưu/đọc playlist + settings vào config.ini cạnh exe.
// Thay thế cho việc bắt buộc đặt file media chung thư mục exe để được scan.
#pragma once
#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>

// Đường dẫn đầy đủ tới config.ini (cạnh exe, tên riêng — không phải scan thư mục).
extern wchar_t g_configPath[MAX_PATH];

// FIX/NEW: path đầy đủ của wallpaper đang được áp dụng, đọc/ghi qua key
// [Settings] CurrentWallpaper trong config.ini. Đây là nguồn chân lý chính
// xác hơn CurrentTrack (index): index có thể lệch giữa Manager và Engine
// vì mỗi bên tự giữ mảng playlist riêng (thứ tự có thể khác nhau — VD
// Manager thêm file theo thứ tự thumbnail sinh xong bất đồng bộ, không
// phải thứ tự gốc lưu trong config.ini). Path thì luôn trỏ đúng 1 file
// vật lý bất kể thứ tự mảng ở 2 bên có khớp hay không.
extern wchar_t g_currentWallpaperPath[MAX_PATH];

// Gọi 1 lần ở đầu WinMain, truyền vào thư mục chứa exe (có dấu \ ở cuối).
// Chỉ build g_configPath, KHÔNG đọc file.
void Config_Init(const wchar_t* exeDir);

// Đọc config.ini -> nạp g_playlist, g_currentTrack, g_volume, g_isLooping,
// g_isMuted, g_isTopMost. File trong playlist không còn tồn tại trên đĩa sẽ
// bị bỏ qua. Trả về false nếu config.ini không tồn tại hoặc playlist rỗng
// sau khi lọc — lúc đó caller nên fallback sang ScanPlaylist() như cũ.
bool Config_Load();

// Ghi toàn bộ state hiện tại (playlist + settings) xuống config.ini.
// An toàn để gọi nhiều lần (mỗi lần overwrite section liên quan).
void Config_Save();

#endif // CONFIG_H
