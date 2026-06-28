// settings.h - Settings UI header
#pragma once

#ifndef SETTINGS_H
#define SETTINGS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

    void ShowSettingsDialog(HWND parent);
    void CloseSettingsDialog();
    bool IsSettingsDialogOpen();
    void UpdateSettingsUI();

#ifdef __cplusplus
}
#endif

#endif // SETTINGS_H
