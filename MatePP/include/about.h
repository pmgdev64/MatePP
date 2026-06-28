// about.h - About dialog for Mate++ Wallpaper Engine
#pragma once
#ifndef ABOUT_H
#define ABOUT_H

#include <windows.h>

class AboutDialog {
public:
    static void Show(HWND parent);

private:
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
};

#endif // ABOUT_H
