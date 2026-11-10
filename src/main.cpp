// Strata - entry point
//
//   Strata.exe              open Strata (or bring the running window forward)
//   Strata.exe <folder>     open a folder in a new tab (a file path opens its folder and selects it)
//   Strata.exe --quit       close a running Strata
#include "common.h"
#include "config.h"
#include "window.h"
#include <shellapi.h>
#include <ole2.h>

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    Log(L"CRASH: exception 0x%08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(CrashHandler);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::vector<std::wstring> paths;
    bool quit = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--quit") quit = true;
        else if (!a.empty() && a.rfind(L"--", 0) != 0) {
            wchar_t full[4096];
            if (GetFullPathNameW(a.c_str(), (DWORD)std::size(full), full, nullptr)) a = full;
            paths.push_back(a);
        }
    }
    if (argv) LocalFree(argv);

    // One Strata process: a second launch hands its folder to the running window.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Strata.larosa023.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(STRATA_WNDCLASS, nullptr);
        if (existing) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            if (quit) {
                PostMessageW(existing, WM_CLOSE, 0, 0);
            } else {
                std::wstring p = paths.empty() ? L"" : paths[0];
                COPYDATASTRUCT cds{0x5354, (DWORD)((p.size() + 1) * sizeof(wchar_t)), (PVOID)p.c_str()};
                SendMessageTimeoutW(existing, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, nullptr);
                for (size_t i = 1; i < paths.size(); ++i) {
                    COPYDATASTRUCT c2{0x5354, (DWORD)((paths[i].size() + 1) * sizeof(wchar_t)), (PVOID)paths[i].c_str()};
                    SendMessageTimeoutW(existing, WM_COPYDATA, 0, (LPARAM)&c2, SMTO_ABORTIFHUNG, 3000, nullptr);
                }
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (quit) return 0;

    OleInitialize(nullptr);   // COM (apartment) + clipboard + drag and drop
    LogInit();
    Log(L"==== Strata %ls starting (data: %ls)", STRATA_VERSION, DataDir().c_str());
    ConfigLoad();

    HWND hwnd = MainCreate(inst, paths);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Strata couldn't start. Details are in strata.log.", L"Strata", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Log(L"Strata exited normally");
    OleUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
