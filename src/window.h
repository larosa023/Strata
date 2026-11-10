// Strata - main window
#pragma once
#include "common.h"

HWND MainCreate(HINSTANCE inst, const std::vector<std::wstring>& openPaths, bool startHidden, const wchar_t* cmdLine);
void ReleaseInstanceMutex();   // main.cpp
void MainOpenPath(const std::wstring& path, bool newTab);   // used when a second Strata.exe is launched
