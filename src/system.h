// Strata - Windows integration: install / uninstall, default file manager (with a safe fallback),
// start with Windows, portable mode, tray icon
#pragma once
#include "common.h"

// ---- start with Windows (starts hidden in the tray for instant opening)
bool StartWithWindowsEnabled();
void SetStartWithWindows(bool on);

// ---- default file manager: Win+E, folder double-clicks and "Open" on folders go to Strata.
// Only the current user's registry is touched; File Explorer keeps working (Ctrl+Shift+Win+E isn't
// affected, virtual folders like Control Panel and the Recycle Bin stay in Explorer), and turning it off
// or uninstalling removes every key. If Strata crashes while opening a folder, Explorer opens it instead,
// and repeated crashes switch the integration off automatically.
bool DefaultFileManagerEnabled();
bool SetDefaultFileManager(bool on, std::wstring* error);
void DefaultFileManagerRepairPath();      // keep the registered path pointing at this exe if it moved
void CrashGuardStartup();                 // called early; disables the integration after repeated crashes
void CrashGuardRecord();                  // from the crash handler
std::wstring CrashGuardMessage();         // non-empty when the integration was switched off for safety

// ---- install / uninstall / portable
bool IsInstalledCopy();                   // running from %LOCALAPPDATA%\Programs\Strata
std::wstring InstallDir();
bool InstallStrata(std::wstring* error);  // copies the exe, adds Start menu shortcut + uninstall entry, relaunches
void UninstallStrata(bool keepSettings);  // removes everything Strata added, then the program itself
bool IsPortable();
bool SetPortable(bool on, std::wstring* error);

// ---- tray
void TrayAdd(HWND owner);
void TrayRemove();
void TrayOnMessage(HWND owner, LPARAM lParam);
