// pipe_server.h - Named Pipe server cho mpp.exe
// Hub gửi lệnh, mpp nhận và xử lý
#pragma once
#ifndef PIPE_SERVER_H
#define PIPE_SERVER_H

#include <windows.h>

void PipeServer_Start();
void PipeServer_Stop();

#endif
