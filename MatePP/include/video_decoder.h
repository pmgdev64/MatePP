#pragma once
// video_decoder.h — FFmpeg video decode backend
// Replaces WMF IMFSourceReader for video decoding
// Thread model: DecodeThread writes to double pixbuf,
//               RenderThread reads front buf + uploads to D2D bitmap

#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <stdint.h>

// ============================================================
//  Public API — called from main.cpp
// ============================================================

// Open file, find video stream, allocate pixbufs + D2D bitmap, start decode thread.
// Must be called with g_pRT valid. Returns false on failure.
bool VidDec_Load(const wchar_t* path, ID2D1RenderTarget* rt);

// Stop decode thread, free all FFmpeg + pixbuf resources.
// Safe to call even if not loaded.
void VidDec_Stop();

// Called from render thread: if a new frame is ready, upload it to D2D bitmap.
// Returns true if bitmap was updated.
bool VidDec_UploadFrame();

// Seek to beginning (for looping). Non-blocking: posts seek request to decode thread.
void VidDec_SeekToStart();

// ============================================================
//  State exposed to main.cpp (read-only)
// ============================================================
extern bool          g_vidLoaded;   // true after VidDec_Load succeeds
extern bool          g_useVideo;    // true = video mode (vs GIF)
extern UINT          g_vidW, g_vidH;
extern DWORD         g_frameDur;    // ms per frame (for debug overlay)
extern ID2D1Bitmap*  g_pVideoBmp;   // D2D bitmap updated by VidDec_UploadFrame

#endif // VIDEO_DECODER_H
