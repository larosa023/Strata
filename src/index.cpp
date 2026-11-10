// Strata - whole-PC index: every file and folder on the fixed drives, kept in memory and on disk
// (index.bin) so search is instant without Everything. Built at low priority in the background,
// refreshed every few hours.
#include "index.h"
#include "config.h"
#include "updater.h"
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <ctime>

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Entry {
    uint32_t parent;       // index of the parent folder entry (UINT32_MAX for drive roots)
    uint32_t nameOff;      // offset into the name pools
    uint16_t nameLen;
    uint16_t flags;        // 1 = folder
    uint64_t size;
    uint64_t modified;     // FILETIME
};
}  // namespace

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct IndexData {
    std::vector<Entry> entries;
    std::wstring names;    // original case
    std::wstring lower;    // lowercase copy for matching
    time_t built = 0;
};
}  // namespace

static std::shared_mutex g_lock;
static IndexData g_data;
static bool g_ready = false;
static std::atomic<bool> g_building{false};
static std::atomic<size_t> g_progress{0};
static std::atomic<bool> g_stop{false};
static HWND g_notify = nullptr;
static bool g_threadStarted = false;

static std::wstring IndexPath() { return DataDir() + L"\\index.bin"; }

bool IndexReady() { return g_ready; }

std::wstring IndexStatusText() {
    if (!g_config.useIndex) return L"The index is off.";
    if (g_building && !g_ready) return L"Building the index… " + std::to_wstring(g_progress.load()) + L" items so far.";
    std::shared_lock<std::shared_mutex> l(g_lock);
    if (!g_ready) return L"The index will build in the background.";
    wchar_t b[64] = L"";
    tm lt{};
    localtime_s(&lt, &g_data.built);
    wcsftime(b, 64, L"%b %d, %I:%M %p", &lt);
    std::wstring s = L"Indexed " + std::to_wstring(g_data.entries.size()) + L" items (updated " + b + L").";
    if (g_building) s += L" Refreshing now…";
    return s;
}

static std::wstring FullPath(const IndexData& d, uint32_t i) {
    std::vector<uint32_t> chain;
    for (uint32_t k = i; k != UINT32_MAX && chain.size() < 512; k = d.entries[k].parent) chain.push_back(k);
    std::wstring p;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Entry& e = d.entries[*it];
        std::wstring nm = d.names.substr(e.nameOff, e.nameLen);
        if (p.empty()) p = nm;   // drive root "C:\"
        else p = PathJoin(p, nm);
    }
    return p;
}

static bool Save(const IndexData& d) {
    HANDLE h = CreateFileW((IndexPath() + L".tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w;
    uint32_t magic = 0x58444E53, ver = 1;   // "SNDX"
    uint64_t n = d.entries.size(), m = d.names.size(), built = (uint64_t)d.built;
    bool ok = WriteFile(h, &magic, 4, &w, nullptr) && WriteFile(h, &ver, 4, &w, nullptr) && WriteFile(h, &built, 8, &w, nullptr) &&
              WriteFile(h, &n, 8, &w, nullptr) && WriteFile(h, &m, 8, &w, nullptr);
    if (ok && n) ok = WriteFile(h, d.entries.data(), (DWORD)(n * sizeof(Entry)), &w, nullptr);
    if (ok && m) ok = WriteFile(h, d.names.data(), (DWORD)(m * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    return MoveFileExW((IndexPath() + L".tmp").c_str(), IndexPath().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

static bool Load(IndexData& d) {
    HANDLE h = CreateFileW(IndexPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD r;
    uint32_t magic = 0, ver = 0;
    uint64_t n = 0, m = 0, built = 0;
    bool ok = ReadFile(h, &magic, 4, &r, nullptr) && magic == 0x58444E53 && ReadFile(h, &ver, 4, &r, nullptr) && ver == 1 &&
              ReadFile(h, &built, 8, &r, nullptr) && ReadFile(h, &n, 8, &r, nullptr) && ReadFile(h, &m, 8, &r, nullptr) &&
              n < 50000000 && m < 2000000000;
    if (ok) {
        d.entries.resize((size_t)n);
        d.names.resize((size_t)m);
        ok = (!n || (ReadFile(h, d.entries.data(), (DWORD)(n * sizeof(Entry)), &r, nullptr) && r == n * sizeof(Entry))) &&
             (!m || (ReadFile(h, &d.names[0], (DWORD)(m * sizeof(wchar_t)), &r, nullptr) && r == m * sizeof(wchar_t)));
        d.built = (time_t)built;
        d.lower = d.names;
        if (!d.lower.empty()) CharLowerBuffW(&d.lower[0], (DWORD)d.lower.size());
    }
    CloseHandle(h);
    return ok;
}

static void Crawl(IndexData& d) {
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW((DWORD)std::size(buf), buf);
    if (!n || n > std::size(buf)) return;
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    for (wchar_t* p = buf; *p && !g_stop; p += wcslen(p) + 1) {
        UINT t = GetDriveTypeW(p);
        if (t != DRIVE_FIXED && !(t == DRIVE_REMOTE && g_config.searchNetwork)) continue;
        Entry root{UINT32_MAX, (uint32_t)d.names.size(), (uint16_t)wcslen(p), 1, 0, 0};
        d.names += p;
        d.entries.push_back(root);
        std::vector<std::pair<uint32_t, std::wstring>> stack{{(uint32_t)d.entries.size() - 1, p}};
        while (!stack.empty() && !g_stop) {
            auto [idx, dir] = stack.back();
            stack.pop_back();
            while (updater::UserIsBusy() && !g_stop) Sleep(3000);   // never compete with games
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW(PathJoin(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                size_t len = wcslen(fd.cFileName);
                if (len > 0xFFFF) continue;
                bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                Entry e{idx, (uint32_t)d.names.size(), (uint16_t)len, (uint16_t)(isDir ? 1 : 0),
                        isDir ? 0 : (((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow),
                        ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime};
                d.names.append(fd.cFileName, len);
                d.entries.push_back(e);
                if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && fd.cFileName[0] != L'$')
                    stack.push_back({(uint32_t)d.entries.size() - 1, PathJoin(dir, fd.cFileName)});
                if ((d.entries.size() & 4095) == 0) g_progress = d.entries.size();
            } while (FindNextFileW(h, &fd) && d.entries.size() < 40000000);
            FindClose(h);
        }
    }
    SetErrorMode(oldMode);
}

static void Builder() {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    {
        IndexData loaded;
        if (Load(loaded)) {
            std::unique_lock<std::shared_mutex> l(g_lock);
            g_data = std::move(loaded);
            g_ready = true;
            PostMessageW(g_notify, WM_STRATA_INDEX, 0, 0);
        }
    }
    for (;;) {
        if (g_stop) return;
        time_t age;
        { std::shared_lock<std::shared_mutex> l(g_lock); age = time(nullptr) - g_data.built; }
        if (!g_ready || age > 6 * 3600) {
            g_building = true;
            g_progress = 0;
            PostMessageW(g_notify, WM_STRATA_INDEX, 0, 0);
            IndexData fresh;
            Crawl(fresh);
            if (g_stop) { g_building = false; return; }
            fresh.built = time(nullptr);
            fresh.lower = fresh.names;
            if (!fresh.lower.empty()) CharLowerBuffW(&fresh.lower[0], (DWORD)fresh.lower.size());
            Save(fresh);
            {
                std::unique_lock<std::shared_mutex> l(g_lock);
                g_data = std::move(fresh);
                g_ready = true;
            }
            g_building = false;
            Log(L"Index built: %u items", (unsigned)g_data.entries.size());
            PostMessageW(g_notify, WM_STRATA_INDEX, 0, 0);
        }
        for (int i = 0; i < 600 && !g_stop; ++i) Sleep(1000);   // check again in 10 minutes
    }
}

void IndexInit(HWND notify) {
    g_notify = notify;
    IndexApplySettings();
}

void IndexApplySettings() {
    if (g_config.useIndex && !g_threadStarted) {
        g_threadStarted = true;
        g_stop = false;
        std::thread(Builder).detach();
    } else if (!g_config.useIndex && g_threadStarted) {
        g_stop = true;
        g_threadStarted = false;
        std::unique_lock<std::shared_mutex> l(g_lock);
        g_data = IndexData();
        g_ready = false;
    }
}

bool IndexQuery(const std::vector<std::wstring>& roots, const SearchQuery& q, size_t max, std::vector<Item>& out, bool& capped) {
    std::shared_lock<std::shared_mutex> l(g_lock);
    if (!g_ready) return false;
    capped = false;
    const IndexData& d = g_data;
    std::vector<std::wstring> rootsLower;
    for (auto& r : roots) rootsLower.push_back(ToLower(r));
    for (uint32_t i = 0; i < d.entries.size(); ++i) {
        const Entry& e = d.entries[i];
        if (e.parent == UINT32_MAX) continue;
        std::wstring nameLower(d.lower, e.nameOff, e.nameLen);
        FILETIME mt{(DWORD)(e.modified & 0xFFFFFFFF), (DWORD)(e.modified >> 32)};
        if (!q.Match(nameLower, e.flags & 1, e.size, mt)) continue;
        std::wstring full = FullPath(d, i);
        if (!rootsLower.empty()) {
            std::wstring fl = ToLower(full);
            bool inside = false;
            for (auto& r : rootsLower) if (fl.size() > r.size() && fl.compare(0, r.size(), r) == 0 && (r.back() == L'\\' || fl[r.size()] == L'\\')) inside = true;
            if (!inside) continue;
        }
        Item it;
        it.name = d.names.substr(e.nameOff, e.nameLen);
        it.path = full;
        it.attrs = (e.flags & 1) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        it.size = e.size;
        it.modified = mt;
        out.push_back(std::move(it));
        if (out.size() >= max) { capped = true; break; }
    }
    return true;
}
