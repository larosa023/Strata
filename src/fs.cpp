// Strata - background folder enumeration, drives, folder watching
#include "fs.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thread>
#include <mutex>
#include <map>
#include <unordered_map>
#include <algorithm>

// ---------------------------------------------------------------- path helpers
static bool IsDriveRoot(const std::wstring& p) { return p.size() == 3 && p[1] == L':' && p[2] == L'\\'; }

std::wstring NormalizeFolder(const std::wstring& in) {
    std::wstring p = in;
    for (auto& c : p) if (c == L'/') c = L'\\';
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() == 3 && p[1] == L':') p[0] = towupper(p[0]);
    return p;
}

std::wstring PathJoin(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

std::wstring PathParent(const std::wstring& path) {
    std::wstring p = NormalizeFolder(path);
    if (p.empty() || IsDriveRoot(p)) return L"";
    if (p.rfind(L"\\\\", 0) == 0) {  // UNC: \\server\share is a root
        size_t s1 = p.find(L'\\', 2);
        size_t s2 = s1 == std::wstring::npos ? s1 : p.find(L'\\', s1 + 1);
        if (s2 == std::wstring::npos) return L"";
    }
    size_t s = p.find_last_of(L'\\');
    if (s == std::wstring::npos) return L"";
    std::wstring parent = p.substr(0, s);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    return parent;
}

std::wstring PathLeaf(const std::wstring& path) {
    std::wstring p = NormalizeFolder(path);
    if (p.empty()) return L"This PC";
    if (IsDriveRoot(p)) return p.substr(0, 2);
    size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

bool IsDirectory(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> SplitPath(const std::wstring& path, std::vector<std::wstring>* fullPaths) {
    std::vector<std::wstring> parts;
    if (fullPaths) fullPaths->clear();
    std::wstring p = NormalizeFolder(path);
    if (p.empty()) return parts;
    std::wstring cur;
    size_t pos = 0;
    if (p.rfind(L"\\\\", 0) == 0) {
        size_t s1 = p.find(L'\\', 2);
        size_t s2 = s1 == std::wstring::npos ? std::wstring::npos : p.find(L'\\', s1 + 1);
        cur = p.substr(0, s2);
        parts.push_back(cur);
        if (fullPaths) fullPaths->push_back(cur);
        pos = s2 == std::wstring::npos ? p.size() : s2 + 1;
    } else if (p.size() >= 2 && p[1] == L':') {
        cur = p.substr(0, 2) + L"\\";
        parts.push_back(p.substr(0, 2));
        if (fullPaths) fullPaths->push_back(cur);
        pos = 3;
    }
    while (pos < p.size()) {
        size_t next = p.find(L'\\', pos);
        std::wstring seg = p.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        if (!seg.empty()) {
            cur = PathJoin(cur, seg);
            parts.push_back(seg);
            if (fullPaths) fullPaths->push_back(cur);
        }
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    return parts;
}

// Long-path aware search pattern
static std::wstring SearchPattern(const std::wstring& folder) {
    std::wstring base = folder;
    if (base.size() > 200 && base.rfind(L"\\\\?\\", 0) != 0) {
        if (base.rfind(L"\\\\", 0) == 0) base = L"\\\\?\\UNC\\" + base.substr(2);
        else base = L"\\\\?\\" + base;
    }
    return PathJoin(base, L"*");
}

std::vector<std::wstring> ListSubfolders(const std::wstring& folder, size_t max, bool includeHidden) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(SearchPattern(folder).c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!includeHidden && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        out.push_back(fd.cFileName);
    } while (out.size() < max && FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

// ---------------------------------------------------------------- formatting
std::wstring FormatSize(uint64_t b) {
    wchar_t buf[64];
    if (b < 1024) { swprintf(buf, 64, L"%llu B", (unsigned long long)b); return buf; }
    const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = (double)b / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    if (v < 10) swprintf(buf, 64, L"%.1f %ls", v, units[u]);
    else swprintf(buf, 64, L"%.0f %ls", v, units[u]);
    return buf;
}

std::wstring FormatDate(const FILETIME& ft) {
    if (!ft.dwHighDateTime && !ft.dwLowDateTime) return L"";
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"";
    wchar_t d[64] = {}, t[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, t, 64);
    return std::wstring(d) + L"  " + t;
}

// ---------------------------------------------------------------- type + icon cache
struct TypeInfo { std::wstring name; int icon = -1; };
static std::mutex g_typeLock;
static std::unordered_map<std::wstring, TypeInfo> g_typeCache;   // key: lowercase extension, "<dir>"

static TypeInfo LookupType(const std::wstring& extLower, bool dir) {
    std::wstring key = dir ? L"<dir>" : extLower;
    {
        std::lock_guard<std::mutex> lock(g_typeLock);
        auto it = g_typeCache.find(key);
        if (it != g_typeCache.end()) return it->second;
    }
    SHFILEINFOW sfi{};
    std::wstring fake = dir ? L"folder" : (L"file" + extLower);
    TypeInfo ti;
    if (SHGetFileInfoW(fake.c_str(), dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_SYSICONINDEX)) {
        ti.name = sfi.szTypeName;
        ti.icon = sfi.iIcon;
    }
    if (ti.name.empty()) {
        if (dir) ti.name = L"File folder";
        else if (extLower.empty()) ti.name = L"File";
        else { std::wstring e = extLower.substr(1); CharUpperBuffW(&e[0], (DWORD)e.size()); ti.name = e + L" File"; }
    }
    std::lock_guard<std::mutex> lock(g_typeLock);
    g_typeCache[key] = ti;
    return ti;
}

int FolderIconIndex() { return LookupType(L"", true).icon; }

static bool NeedsRealIcon(const std::wstring& extLower, DWORD attrs) {
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM)) != 0;  // desktop.ini folders
    static const wchar_t* kExts[] = {L".exe", L".lnk", L".ico", L".url", L".cur", L".ani", L".msc", L".cpl", L".scr", L".appref-ms"};
    for (auto e : kExts) if (extLower == e) return true;
    return false;
}

static std::wstring ExtLower(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    return ToLower(name.substr(dot));
}

// ---------------------------------------------------------------- drives
std::wstring DriveDisplayName(const DriveInfo& d) {
    std::wstring letter = d.root.substr(0, 2);
    std::wstring label = d.label;
    if (label.empty()) {
        switch (d.type) {
            case DRIVE_REMOVABLE: label = L"USB Drive"; break;
            case DRIVE_REMOTE: label = L"Network Drive"; break;
            case DRIVE_CDROM: label = L"DVD Drive"; break;
            case DRIVE_RAMDISK: label = L"RAM Disk"; break;
            default: label = L"Local Disk"; break;
        }
    }
    return label + L" (" + letter + L")";
}

static std::vector<DriveInfo> EnumDrives() {
    std::vector<DriveInfo> drives;
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW((DWORD)std::size(buf), buf);
    if (!n || n > std::size(buf)) return drives;
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);   // no "insert a disk" dialogs
    for (wchar_t* p = buf; *p; p += wcslen(p) + 1) {
        DriveInfo d;
        d.root = p;
        d.type = GetDriveTypeW(p);
        if (d.type == DRIVE_NO_ROOT_DIR) continue;
        ULARGE_INTEGER freeToCaller{}, total{}, totalFree{};
        if (GetDiskFreeSpaceExW(p, &freeToCaller, &total, &totalFree)) {
            d.ready = true;
            d.total = total.QuadPart;
            d.freeBytes = totalFree.QuadPart;
            wchar_t label[MAX_PATH + 1] = {};
            if (GetVolumeInformationW(p, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) d.label = label;
        }
        if (d.type == DRIVE_REMOTE && d.label.empty()) {
            wchar_t remote[MAX_PATH] = {};
            DWORD len = MAX_PATH;
            std::wstring local = d.root.substr(0, 2);
            if (WNetGetConnectionW(local.c_str(), remote, &len) == NO_ERROR) {
                std::wstring r = remote;
                size_t s = r.find_last_of(L'\\');
                d.label = s == std::wstring::npos ? r : r.substr(s + 1);
            }
        }
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(p, 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX)) d.icon = sfi.iIcon;
        drives.push_back(d);
    }
    SetErrorMode(oldMode);
    return drives;
}

void ListDrivesAsync(HWND notify) {
    std::thread([notify] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto* drives = new std::vector<DriveInfo>(EnumDrives());
        if (!PostMessageW(notify, WM_STRATA_DRIVES, 0, (LPARAM)drives)) delete drives;
        CoUninitialize();
    }).detach();
}

// ---------------------------------------------------------------- folder listing
static std::mutex g_genLock;
static std::map<int, uint64_t> g_latestGen;   // tab -> latest requested generation

static bool Superseded(int tab, uint64_t gen) {
    std::lock_guard<std::mutex> lock(g_genLock);
    auto it = g_latestGen.find(tab);
    return it != g_latestGen.end() && it->second != gen;
}

void ListFolderAsync(HWND notify, int tab, uint64_t gen, const std::wstring& folder) {
    {
        std::lock_guard<std::mutex> lock(g_genLock);
        g_latestGen[tab] = gen;
    }
    std::thread([notify, tab, gen, folder] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);

        auto* L = new Listing;
        L->tab = tab;
        L->gen = gen;
        L->folder = folder;
        std::map<std::wstring, int> typeIndex;
        auto typeOf = [&](const std::wstring& name) -> int {
            auto it = typeIndex.find(name);
            if (it != typeIndex.end()) return it->second;
            int idx = (int)L->types.size();
            L->types.push_back(name);
            typeIndex[name] = idx;
            return idx;
        };
        std::vector<int> special;

        if (folder.empty()) {
            // This PC: drives as rows
            for (auto& d : EnumDrives()) {
                Item it;
                it.name = DriveDisplayName(d);
                it.path = d.root;
                it.attrs = FILE_ATTRIBUTE_DIRECTORY;
                it.icon = d.icon;
                it.total = d.total;
                it.freeBytes = d.freeBytes;
                it.size = d.total;
                const wchar_t* tn = d.type == DRIVE_REMOTE ? L"Network Drive" : d.type == DRIVE_REMOVABLE ? L"Removable Disk"
                                  : d.type == DRIVE_CDROM ? L"CD Drive" : L"Local Disk";
                it.type = typeOf(tn);
                L->items.push_back(std::move(it));
            }
        } else {
            UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW(SearchPattern(folder).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                        nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) {
                L->error = GetLastError();
                if (L->error == ERROR_FILE_NOT_FOUND) L->error = 0;   // empty drive root
            } else {
                size_t n = 0;
                do {
                    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                    Item it;
                    it.name = fd.cFileName;
                    it.attrs = fd.dwFileAttributes;
                    it.modified = fd.ftLastWriteTime;
                    bool dir = it.isDir();
                    it.size = dir ? 0 : ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    std::wstring ext = dir ? L"" : ExtLower(it.name);
                    TypeInfo ti = LookupType(ext, dir);
                    it.type = typeOf(ti.name);
                    it.icon = ti.icon;
                    if (NeedsRealIcon(ext, it.attrs)) special.push_back((int)L->items.size());
                    L->items.push_back(std::move(it));
                    if ((++n & 1023) == 0 && Superseded(tab, gen)) break;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            SetErrorMode(oldMode);
        }
        QueryPerformanceCounter(&t1);
        L->ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;

        // Copy what the icon pass needs before handing the listing over
        std::vector<std::wstring> specialNames;
        for (int i : special) specialNames.push_back(L->items[i].name);
        if (Superseded(tab, gen) || !PostMessageW(notify, WM_STRATA_LISTING, 0, (LPARAM)L)) {
            delete L;
            CoUninitialize();
            return;
        }

        // Second pass: real icons for exe / lnk / ico / customized folders
        IconBatch* batch = nullptr;
        for (size_t k = 0; k < special.size(); ++k) {
            if ((k & 31) == 0 && Superseded(tab, gen)) break;
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(PathJoin(folder, specialNames[k]).c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX)) {
                if (!batch) { batch = new IconBatch; batch->tab = tab; batch->gen = gen; }
                batch->icons.push_back({special[k], sfi.iIcon});
            }
            if (batch && (batch->icons.size() >= 64 || k + 1 == special.size())) {
                if (!PostMessageW(notify, WM_STRATA_ICONS, 0, (LPARAM)batch)) delete batch;
                batch = nullptr;
            }
        }
        delete batch;
        CoUninitialize();
    }).detach();
}

// ---------------------------------------------------------------- folder watcher
static std::atomic<uint64_t> g_watchGen{0};
static HANDLE g_watchStop = nullptr;

uint64_t WatchGeneration() { return g_watchGen.load(); }

void WatchFolder(HWND notify, const std::wstring& folder) {
    if (g_watchStop) { SetEvent(g_watchStop); CloseHandle(g_watchStop); g_watchStop = nullptr; }
    uint64_t gen = ++g_watchGen;
    if (folder.empty()) return;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stopForThread = nullptr;
    DuplicateHandle(GetCurrentProcess(), stop, GetCurrentProcess(), &stopForThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_watchStop = stop;
    std::thread([notify, folder, gen, stopForThread] {
        HANDLE ch = FindFirstChangeNotificationW(folder.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES);
        if (ch != INVALID_HANDLE_VALUE) {
            HANDLE waits[2] = {stopForThread, ch};
            while (true) {
                DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (r != WAIT_OBJECT_0 + 1) break;
                if (g_watchGen.load() != gen) break;
                PostMessageW(notify, WM_STRATA_DIRCHANGED, (WPARAM)gen, 0);
                if (!FindNextChangeNotification(ch)) break;
            }
            FindCloseChangeNotification(ch);
        }
        CloseHandle(stopForThread);
    }).detach();
}
