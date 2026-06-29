#pragma once
// audio_system.h — WASAPI + Media Foundation audio playback
// Reads audio stream from same video file as video decoder
// Thread-safe: audio runs on its own dedicated thread

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmsystem.h>

// ============================================================
//  Audio System API
// ============================================================

// Khởi động audio system với file path (cùng file video)
// gọi sau LoadVideo thành công
bool AudioSystem_Start(const wchar_t* path, float volume);

// Dừng và giải phóng toàn bộ audio resources
void AudioSystem_Stop();

// Pause / Resume
void AudioSystem_SetPaused(bool paused);

// Volume: 0.0 - 1.0
void AudioSystem_SetVolume(float volume);

// Mute toggle
void AudioSystem_SetMuted(bool muted);

// Seek về đầu (gọi khi video loop)
void AudioSystem_Seek(LONGLONG positionHns); // đơn vị 100ns, 0 = đầu file

// Trạng thái
bool AudioSystem_IsRunning();
