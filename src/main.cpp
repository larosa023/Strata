// Strata - entry point
//
//   Strata.exe                    open Strata (or bring the running window forward)
//   Strata.exe <folder>           open a folder in a new tab (a file path opens its folder and selects it)
//   Strata.exe --tray             start hidden in the tray (Start with Windows)
//   Strata.exe --quit             close a running Strata
//   Strata.exe --uninstall        remove Strata (used by Apps & features)
//   Strata.exe --restore-explorer stop opening folders with Strata (undo "default file manager")
//   (--updated <pid> / --rolled-back <version> / --wait-old are used by the updater and installer)
#include "common.h"
#include "config.h"
#include "window.h"
#include "system.h"
#include <shellapi.h>
#include <ole2.h>

static std::wstring g_startupFolder;   // folder Strata was asked to open (for the crash fallback)
static DWORD g_startTick = 0;
static HANDLE g_mutex = nullptr;

void ReleaseInstanceMutex() {
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    Log(L"CRASH: exception 0x%08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    CrashGuardRecord();
    // Safe fallback: if we were opening a folder for Windows, let File Explorer open it instead
    if (!g_startupFolder.empty() && GetTickCount() - g_startTick < 15000) {
        std::wstring args = L"\"" + g_startupFolder + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_startTick = GetTickCount();
#ifndef STRATA_NO_CRASH_HANDLER
    SetUnhandledExceptionFilter(CrashHandler);
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::vector<std::wstring> paths;
    bool quit = false, tray = false, uninstall = false, restoreExplorer = false, waitOld = false;
    NewWindowArgs nw;
    auto after = [](const std::wstring& a, const wchar_t* key) { size_t n = wcslen(key); return a.compare(0, n, key) == 0 ? a.substr(n) : std::wstring(); };
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--quit") quit = true;
        else if (a == L"--tray") tray = true;
        else if (a == L"--uninstall") uninstall = true;
        else if (a == L"--restore-explorer") restoreExplorer = true;
        else if (a == L"--wait-old") waitOld = true;
        else if (a == L"--updated" || a == L"--rolled-back") ++i;   // value handled by the updater
        else if (a == L"--new-window") nw.active = true;
        else if (a.rfind(L"--left=", 0) == 0) nw.left = after(a, L"--left=");
        else if (a.rfind(L"--right=", 0) == 0) nw.right = after(a, L"--right=");
        else if (a.rfind(L"--pane=", 0) == 0) nw.pane = _wtoi(after(a, L"--pane=").c_str()) & 1;
        else if (a.rfind(L"--color=", 0) == 0) nw.color = (uint32_t)wcstoul(after(a, L"--color=").c_str(), nullptr, 10);
        else if (a == L"--locked") nw.locked = true;
        else if (a.rfind(L"--rect=", 0) == 0) {
            int x = 0, y = 0, w = 0, h = 0;
            if (swscanf_s(after(a, L"--rect=").c_str(), L"%d,%d,%d,%d", &x, &y, &w, &h) == 4) nw.rect = RECT{x, y, x + w, y + h};
        }
        else if (!a.empty() && a.rfind(L"--", 0) != 0) {
            wchar_t full[4096];
            if (GetFullPathNameW(a.c_str(), (DWORD)std::size(full), full, nullptr)) a = full;
            paths.push_back(a);
        }
    }
    if (argv) LocalFree(argv);
    if (!paths.empty()) g_startupFolder = paths[0];

    if (restoreExplorer) {
        OleInitialize(nullptr);
        LogInit();
        ConfigLoad();
        SetDefaultFileManager(false, nullptr);
        MessageBoxW(nullptr, L"Folders will open in File Explorer again.", L"Strata", MB_ICONINFORMATION);
        return 0;
    }

    if (nw.active) {
        // a tab dragged out of a Strata window: its own window (and process), no hand-off
        OleInitialize(nullptr);
        LogInit();
        ConfigLoad();
        g_configSecondaryWindow = true;
        MainSetNewWindow(nw);
        HWND hw = MainCreate(inst, {}, false, GetCommandLineW());
        if (!hw) return 1;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        OleUninitialize();
        return 0;
    }

    // One Strata process: a second launch hands its folder to the running window.
    // (After an update or install the new copy waits for the old one to let go.)
    bool updating = wcsstr(GetCommandLineW(), L"--updated") != nullptr;
    for (int attempt = 0; attempt < ((waitOld || updating) ? 50 : 1); ++attempt) {
        g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\Strata.larosa023.SingleInstance");
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (!(waitOld || updating)) {
            HWND existing = FindWindowW(STRATA_WNDCLASS, nullptr);
            if (existing) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existing, &pid);
                AllowSetForegroundWindow(pid);
                if (quit || uninstall) {
                    PostMessageW(existing, WM_CLOSE, 1, 0);
                    if (uninstall) Sleep(1500);
                } else {
                    auto send = [&](const std::wstring& p) {
                        COPYDATASTRUCT cds{0x5354, (DWORD)((p.size() + 1) * sizeof(wchar_t)), (PVOID)p.c_str()};
                        DWORD_PTR r = 0;
                        if (!SendMessageTimeoutW(existing, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &r) && !p.empty()) {
                            // the running Strata isn't responding: let File Explorer open the folder
                            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                        }
                    };
                    if (paths.empty()) send(L"");
                    for (auto& p : paths) send(p);
                }
            }
            if (g_mutex) CloseHandle(g_mutex);
            if (!uninstall) return 0;
            g_mutex = nullptr;
            break;
        }
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        Sleep(200);
    }
    if (quit) return 0;

    OleInitialize(nullptr);   // COM (apartment) + clipboard + drag and drop
    LogInit();
    Log(L"==== Strata %ls starting (data: %ls)", STRATA_VERSION, DataDir().c_str());
    ConfigLoad();

    if (uninstall) {
        int r = MessageBoxW(nullptr, L"Uninstall Strata?\n\nYes removes Strata but keeps your settings, pins and tags.\nNo removes your settings too.",
                            L"Strata", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r != IDCANCEL) UninstallStrata(r == IDYES);
        return 0;
    }
    CrashGuardStartup();

    HWND hwnd = MainCreate(inst, paths, tray, GetCommandLineW());
    if (!hwnd) {
        if (!g_startupFolder.empty()) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + g_startupFolder + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else MessageBoxW(nullptr, L"Strata couldn't start. Details are in strata.log.", L"Strata", MB_ICONERROR);
        return 1;
    }


    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Log(L"Strata exited normally");
    OleUninitialize();
    ReleaseInstanceMutex();
    return 0;
}
