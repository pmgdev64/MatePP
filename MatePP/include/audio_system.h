#pragma once
// audio_system.h — FFmpeg + WASAPI audio playback
// Decode audio stream từ cùng file với video_decoder (FFmpeg API)
// Không dùng Media Foundation.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmsystem.h>

// ============================================================
//  Audio System API
// ============================================================

// Khởi động audio system với cùng file path đã load video.
// Gọi SAU LoadVideo thành công. COM phải được init trước (main.cpp lo).
// volume: 0.0 - 1.0
bool AudioSystem_Start(const wchar_t* path, float volume);

// Dừng hoàn toàn, free mọi resource. Safe khi chưa start.
void AudioSystem_Stop();

// Pause / Resume — gọi mỗi khi g_isPaused thay đổi
void AudioSystem_SetPaused(bool paused);

// Volume 0.0–1.0 — gọi mỗi khi g_volume thay đổi
void AudioSystem_SetVolume(float volume);

// Mute — gọi mỗi khi g_isMuted thay đổi
void AudioSystem_SetMuted(bool muted);

// Gọi từ video_decoder khi video loop (AVERROR_EOF + g_isLooping).
// Audio thread sẽ seek về đầu đồng bộ.
void AudioSystem_NotifyVideoLoop();

// Query trạng thái
bool AudioSystem_IsRunning();

// True CHỈ KHI audio đã thực sự đẩy được ít nhất 1 block PCM thật ra thiết bị
// (không phải chỉ mới được yêu cầu Start()). Video decoder PHẢI dùng cái này
// thay vì AudioSystem_IsRunning() để quyết định lúc nào an toàn chuyển sang
// audio-master-clock — tránh sync theo clock=0 giả trong lúc audio còn đang
// mở file/WASAPI, gây Sleep() cộng dồn dẫn tới khựng frame đầu.
bool AudioSystem_HasValidClock();

// Vị trí phát hiện tại của audio, tính bằng giây kể từ đầu file (master clock).
// Video decoder dùng giá trị này để đồng bộ thay vì dùng QPC wall-clock riêng.
// Trả về 0.0 nếu audio chưa chạy hoặc chưa có dữ liệu nào được đẩy ra loa.
double AudioSystem_GetClockSec();
