// pipe_server.cpp - Named Pipe server cho mpp.exe
// Nhận lệnh từ mpp_hub.exe và gọi hàm tương ứng
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>
#include "common.h"
#include "pipe_server.h"

#define PIPE_NAME    L"\\\\.\\pipe\\MatePPHub"
#define PIPE_BUFSIZE 4096

static HANDLE              g_hPipeThread = NULL;
static volatile bool       g_pipeRunning = false;

// ============================================================
//  Command handler — called inside pipe thread
// ============================================================
static void HandleCommand(const wchar_t* cmd) {
    LogToFile("[Pipe] Cmd: %ws", cmd);

    if (wcsncmp(cmd, L"LOAD:", 5) == 0) {
        // Load một file ngay lập tức
        const wchar_t* path = cmd + 5;
        // Tìm index trong playlist, nếu không có thì add rồi load
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_playlistMtx);
            for (int i = 0; i < (int)g_playlist.size(); i++) {
                if (_wcsicmp(g_playlist[i].c_str(), path) == 0) {
                    g_currentTrack = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                g_playlist.push_back(path);
                g_currentTrack = (int)g_playlist.size() - 1;
            }
        }
        LoadMediaByIndex(g_currentTrack);
        LogToFile("[Pipe] LOAD -> track %d", g_currentTrack);
    }
    else if (wcsncmp(cmd, L"PLAYLIST:", 9) == 0) {
        // Thêm vào playlist không load ngay
        const wchar_t* path = cmd + 9;
        std::lock_guard<std::mutex> lk(g_playlistMtx);
        g_playlist.push_back(path);
        LogToFile("[Pipe] PLAYLIST add: %ws", path);
    }
    else if (wcscmp(cmd, L"CLEAR") == 0) {
        std::lock_guard<std::mutex> lk(g_playlistMtx);
        g_playlist.clear();
        g_currentTrack = 0;
        LogToFile("[Pipe] CLEAR playlist");
    }
    else if (wcscmp(cmd, L"PAUSE") == 0) {
        g_isPaused = true;
    }
    else if (wcscmp(cmd, L"PLAY") == 0) {
        g_isPaused = false;
    }
    else if (wcscmp(cmd, L"NEXT") == 0) {
        LoadNextMedia();
    }
    else if (wcscmp(cmd, L"PREV") == 0) {
        LoadPrevMedia();
    }
}

// ============================================================
//  Pipe server thread — loop: create → connect → read → reply
// ============================================================
static DWORD WINAPI PipeServerThread(LPVOID) {
    LogToFile("[Pipe] Server thread started");

    while (g_pipeRunning) {
        HANDLE hPipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,              // max instances
            PIPE_BUFSIZE,
            PIPE_BUFSIZE,
            0,
            NULL
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            LogToFile("[Pipe] CreateNamedPipe failed: %d", GetLastError());
            Sleep(1000);
            continue;
        }

        // Wait for hub to connect (blocking)
        BOOL connected = ConnectNamedPipe(hPipe, NULL)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected || !g_pipeRunning) {
            CloseHandle(hPipe);
            continue;
        }

        // Read command (wide string)
        wchar_t buf[PIPE_BUFSIZE / sizeof(wchar_t)] = {};
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &bytesRead, NULL);

        if (ok && bytesRead > 0) {
            HandleCommand(buf);
            // Send ACK
            const wchar_t* ack = L"OK";
            DWORD written;
            WriteFile(hPipe, ack, (DWORD)((wcslen(ack) + 1) * sizeof(wchar_t)), &written, NULL);
        }

        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }

    LogToFile("[Pipe] Server thread stopped");
    return 0;
}

// ============================================================
//  Public API
// ============================================================
void PipeServer_Start() {
    g_pipeRunning = true;
    g_hPipeThread = CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);
    if (g_hPipeThread)
        LogToFile("[Pipe] Server started on %ws", PIPE_NAME);
    else
        LogToFile("[Pipe] Failed to start server thread");
}

void PipeServer_Stop() {
    g_pipeRunning = false;
    // Kick the blocking ConnectNamedPipe bằng cách connect rồi đóng
    HANDLE hKick = CreateFileW(PIPE_NAME, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hKick != INVALID_HANDLE_VALUE) CloseHandle(hKick);
    if (g_hPipeThread) {
        WaitForSingleObject(g_hPipeThread, 2000);
        CloseHandle(g_hPipeThread);
        g_hPipeThread = NULL;
    }
    LogToFile("[Pipe] Server stopped");
}
