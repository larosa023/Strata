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

#define STRATA_VERSION L"0.3.0"
#define STRATA_WNDCLASS L"StrataMain"

// Messages posted to the main window
constexpr UINT WM_STRATA_LISTING    = WM_APP + 1;  // lParam: Listing* (folder enumeration finished)
constexpr UINT WM_STRATA_ICONS      = WM_APP + 2;  // lParam: IconBatch* (real icons for exe/lnk/ico...)
constexpr UINT WM_STRATA_DRIVES     = WM_APP + 3;  // lParam: std::vector<DriveInfo>*
constexpr UINT WM_STRATA_DIRCHANGED = WM_APP + 4;  // wParam: watcher generation
constexpr UINT WM_STRATA_OPDONE     = WM_APP + 5;  // lParam: OpResult*
constexpr UINT WM_STRATA_SEARCH     = WM_APP + 6;  // lParam: SearchBatch*
constexpr UINT WM_STRATA_QUEUE      = WM_APP + 7;  // transfer queue progress changed
constexpr UINT WM_STRATA_QUEUEASK   = WM_APP + 8;  // lParam: QueueAsk* (sent; answer before returning)
constexpr UINT WM_STRATA_JOBDONE    = WM_APP + 9;  // lParam: JobResult*
constexpr UINT WM_STRATA_SIZES      = WM_APP + 10; // folder sizes computed
constexpr UINT WM_STRATA_UPDATE     = WM_APP + 11; // updater status changed
constexpr UINT WM_STRATA_PREVIEW    = WM_APP + 12; // lParam: PreviewResult*
constexpr UINT WM_STRATA_THUMB      = WM_APP + 13; // lParam: ThumbResult*
constexpr UINT WM_STRATA_GIT        = WM_APP + 14; // lParam: GitResult*
constexpr UINT WM_STRATA_TRAY       = WM_APP + 15; // tray icon callback
constexpr UINT WM_STRATA_INDEX      = WM_APP + 16; // whole-PC index progress

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
