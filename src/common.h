// Strata - shared declarations
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <iterator>

#define STRATA_VERSION L"0.1.0"
#define STRATA_WNDCLASS L"StrataMain"

// Messages posted to the main window
constexpr UINT WM_STRATA_LISTING    = WM_APP + 1;  // lParam: Listing* (folder enumeration finished)
constexpr UINT WM_STRATA_ICONS      = WM_APP + 2;  // lParam: IconBatch* (real icons for exe/lnk/ico...)
constexpr UINT WM_STRATA_DRIVES     = WM_APP + 3;  // lParam: std::vector<DriveInfo>*
constexpr UINT WM_STRATA_DIRCHANGED = WM_APP + 4;  // wParam: watcher generation
constexpr UINT WM_STRATA_OPDONE     = WM_APP + 5;  // lParam: OpResult*
constexpr UINT WM_STRATA_SUBFOLDERS = WM_APP + 6;  // reserved

// ---- log.cpp ----
void LogInit();
void Log(const wchar_t* fmt, ...);
std::wstring DataDir();          // %APPDATA%\Strata, or exe folder\data in portable mode
std::wstring ExeDir();
std::wstring ExePath();

// ---- util ----
inline std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
