// Strata - Windows integration (see system.h)
#include "system.h"
#include "config.h"
#include "fs.h"
#include <shlobj.h>
#include <shellapi.h>
#include <ctime>

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Strata";
static const wchar_t* kWinECommand = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command";
static const wchar_t* kWinEKey = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}";
static const wchar_t* kDirShell = L"Software\\Classes\\Directory\\shell";
static const wchar_t* kDriveShell = L"Software\\Classes\\Drive\\shell";

static bool SetStr(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& v) {
    HKEY h;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static std::wstring GetStr(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

// ---------------------------------------------------------------- start with Windows
bool StartWithWindowsEnabled() { return !GetStr(HKEY_CURRENT_USER, kRunKey, L"Strata").empty(); }

void SetStartWithWindows(bool on) {
    if (on) SetStr(HKEY_CURRENT_USER, kRunKey, L"Strata", L"\"" + ExePath() + L"\" --tray");
    else {
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, L"Strata");
            RegCloseKey(h);
        }
    }
}

// ---------------------------------------------------------------- default file manager
bool DefaultFileManagerEnabled() {
    return GetStr(HKEY_CURRENT_USER, kDirShell, nullptr) == L"strata";
}

static void NotifyShell() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

static bool WriteDefaultKeys() {
    std::wstring exe = ExePath();
    std::wstring open = L"\"" + exe + L"\" \"%1\"";
    bool ok = true;
    for (const wchar_t* base : {kDirShell, kDriveShell}) {
        std::wstring b = base;
        ok &= SetStr(HKEY_CURRENT_USER, base, nullptr, L"strata");
        ok &= SetStr(HKEY_CURRENT_USER, (b + L"\\strata").c_str(), nullptr, L"Open in Strata");
        ok &= SetStr(HKEY_CURRENT_USER, (b + L"\\strata").c_str(), L"Icon", L"\"" + exe + L"\",0");
        ok &= SetStr(HKEY_CURRENT_USER, (b + L"\\strata\\command").c_str(), nullptr, open);
    }
    // Win+E
    ok &= SetStr(HKEY_CURRENT_USER, kWinECommand, nullptr, L"\"" + exe + L"\"");
    ok &= SetStr(HKEY_CURRENT_USER, kWinECommand, L"DelegateExecute", L"");
    return ok;
}

bool SetDefaultFileManager(bool on, std::wstring* error) {
    if (on) {
        if (!WriteDefaultKeys()) {
            if (error) *error = L"Couldn't write the settings to the registry.";
            SetDefaultFileManager(false, nullptr);
            return false;
        }
        Log(L"Strata set as the default file manager");
    } else {
        for (const wchar_t* base : {kDirShell, kDriveShell}) {
            std::wstring b = base;
            RegDeleteTreeW(HKEY_CURRENT_USER, (b + L"\\strata").c_str());
            HKEY h;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, base, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &h) == ERROR_SUCCESS) {
                if (GetStr(HKEY_CURRENT_USER, base, nullptr) == L"strata") RegDeleteValueW(h, nullptr);
                RegCloseKey(h);
            }
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, kWinEKey);
        Log(L"Strata is no longer the default file manager");
    }
    g_config.defaultFileManager = on;
    ConfigSave();
    NotifyShell();
    return true;
}

void DefaultFileManagerRepairPath() {
    if (!DefaultFileManagerEnabled()) return;
    std::wstring cur = GetStr(HKEY_CURRENT_USER, (std::wstring(kDirShell) + L"\\strata\\command").c_str(), nullptr);
    if (cur.find(ExePath()) == std::wstring::npos) {
        // Strata moved (or a portable copy is running): only repair if the registered exe no longer exists
        size_t q = cur.find(L'"', 1);
        std::wstring registered = q != std::wstring::npos ? cur.substr(1, q - 1) : L"";
        if (GetFileAttributesW(registered.c_str()) == INVALID_FILE_ATTRIBUTES) {
            WriteDefaultKeys();
            NotifyShell();
            Log(L"Default file manager path repaired -> %ls", ExePath().c_str());
        }
    }
}

// Crash guard: if Strata crashes 3 times within 10 minutes while it's the default file manager,
// hand folders back to Explorer automatically.
static std::wstring CrashLogPath() { return DataDir() + L"\\crashes.txt"; }
static std::wstring g_guardMessage;

void CrashGuardRecord() {
    HANDLE h = CreateFileW(CrashLogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[32];
    int n = snprintf(line, sizeof(line), "%lld\n", (long long)time(nullptr));
    DWORD w;
    WriteFile(h, line, n, &w, nullptr);
    CloseHandle(h);
}

void CrashGuardStartup() {
    std::wstring text;
    if (!ReadUtf8File(CrashLogPath(), text)) return;
    long long now = (long long)time(nullptr);
    int recent = 0;
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find(L'\n', i);
        long long t = _wtoi64(text.substr(i, nl == std::wstring::npos ? std::wstring::npos : nl - i).c_str());
        if (now - t < 600) ++recent;
        i = nl == std::wstring::npos ? text.size() : nl + 1;
    }
    if (recent >= 3 && DefaultFileManagerEnabled()) {
        SetDefaultFileManager(false, nullptr);
        g_guardMessage = L"Strata crashed several times, so folders open in File Explorer again. You can turn this back on in Settings > General.";
        Log(L"Crash guard: default file manager switched off after %d recent crashes", recent);
        DeleteFileW(CrashLogPath().c_str());
    }
    if (recent == 0) DeleteFileW(CrashLogPath().c_str());
}

std::wstring CrashGuardMessage() { return g_guardMessage; }

// ---------------------------------------------------------------- install / uninstall
std::wstring InstallDir() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProgramFiles, KF_FLAG_CREATE, nullptr, &p))) r = std::wstring(p) + L"\\Strata";
    CoTaskMemFree(p);
    return r;
}

bool IsInstalledCopy() {
    std::wstring d = InstallDir();
    return !d.empty() && _wcsicmp(ExeDir().c_str(), d.c_str()) == 0;
}

static std::wstring ShortcutPath() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p))) r = std::wstring(p) + L"\\Strata.lnk";
    CoTaskMemFree(p);
    return r;
}

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target) {
    IShellLinkW* link = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(PathParent(target).c_str());
        link->SetDescription(L"Strata - a fast, modern file manager");
        link->SetIconLocation(target.c_str(), 0);
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    }
    if (pf) pf->Release();
    if (link) link->Release();
    return ok;
}

bool InstallStrata(std::wstring* error) {
    std::wstring dir = InstallDir();
    if (dir.empty()) { if (error) *error = L"Couldn't find your programs folder."; return false; }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring target = dir + L"\\Strata.exe";
    if (_wcsicmp(target.c_str(), ExePath().c_str()) != 0) {
        // an older installed copy may be running: move it aside first
        MoveFileExW(target.c_str(), (target + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!CopyFileW(ExePath().c_str(), target.c_str(), FALSE)) {
            if (error) *error = L"Couldn't copy Strata into " + dir + L" (error " + std::to_wstring(GetLastError()) + L").";
            return false;
        }
    }
    MakeShortcut(ShortcutPath(), target);
    SetStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", L"Strata");
    SetStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", STRATA_VERSION);
    SetStr(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", L"larosa023");
    SetStr(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", target + L",0");
    SetStr(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir);
    SetStr(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString", L"\"" + target + L"\" --uninstall");
    SetStr(HKEY_CURRENT_USER, kUninstallKey, L"URLInfoAbout", L"https://github.com/larosa023/Strata");
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        DWORD one = 1, kb = 1024;
        RegSetValueExW(h, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegSetValueExW(h, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegSetValueExW(h, L"EstimatedSize", 0, REG_DWORD, (const BYTE*)&kb, sizeof(kb));
        RegCloseKey(h);
    }
    // settings that point at the exe follow it to its new home
    bool startup = StartWithWindowsEnabled();
    if (startup) SetStr(HKEY_CURRENT_USER, kRunKey, L"Strata", L"\"" + target + L"\" --tray");
    Log(L"Installed to %ls", dir.c_str());
    return true;
}

void UninstallStrata(bool keepSettings) {
    SetDefaultFileManager(false, nullptr);
    SetStartWithWindows(false);
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    std::wstring dir = ExeDir();
    std::wstring data = DataDir();
    // delete the running exe (and settings, if asked) once this process has exited
    std::wstring cmd = L"cmd.exe /c timeout /t 2 /nobreak >nul & del /f /q \"" + ExePath() + L"\" \"" + ExePath() + L".old\" & rmdir \"" + dir + L"\"";
    if (!keepSettings && !IsPortable()) cmd += L" & rmdir /s /q \"" + data + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    Log(L"Uninstalled");
}

bool IsPortable() {
    return GetFileAttributesW((ExeDir() + L"\\portable").c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesW((ExeDir() + L"\\portable.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SetPortable(bool on, std::wstring* error) {
    std::wstring marker = ExeDir() + L"\\portable";
    std::wstring from = DataDir();
    if (on) {
        std::wstring to = ExeDir() + L"\\data";
        SHCreateDirectoryExW(nullptr, to.c_str(), nullptr);
        for (const wchar_t* f : {L"config.json", L"views.json", L"tags.json"})
            CopyFileW((from + L"\\" + f).c_str(), (to + L"\\" + f).c_str(), FALSE);
        HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { if (error) *error = L"Strata's folder isn't writable, so it can't run portable from there."; return false; }
        CloseHandle(h);
    } else {
        PWSTR appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
            std::wstring to = std::wstring(appData) + L"\\Strata";
            SHCreateDirectoryExW(nullptr, to.c_str(), nullptr);
            for (const wchar_t* f : {L"config.json", L"views.json", L"tags.json"})
                CopyFileW((from + L"\\" + f).c_str(), (to + L"\\" + f).c_str(), FALSE);
        }
        CoTaskMemFree(appData);
        DeleteFileW(marker.c_str());
        DeleteFileW((ExeDir() + L"\\portable.txt").c_str());
    }
    return true;
}

// ---------------------------------------------------------------- tray
static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;

void TrayAdd(HWND owner) {
    if (g_trayAdded) return;
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = owner;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_STRATA_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(g_nid.szTip, L"Strata");
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    if (g_trayAdded) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    }
}

void TrayRemove() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}
