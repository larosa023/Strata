// Strata - main window
#pragma once
#include "common.h"

HWND MainCreate(HINSTANCE inst, const std::vector<std::wstring>& openPaths, bool startHidden, const wchar_t* cmdLine);
void ReleaseInstanceMutex();   // main.cpp
void MainOpenPath(const std::wstring& path, bool newTab);   // used when a second Strata.exe is launched

// A window made by dragging a tab out of another one (Strata.exe --new-window …)
struct NewWindowArgs {
    bool active = false;
    RECT rect{};
    std::wstring left, right;
    int pane = 0;
    uint32_t color = 0;
    bool locked = false;
};
void MainSetNewWindow(const NewWindowArgs& a);
