// Strata - background folder sizes
#include "sizes.h"
#include "fs.h"
#include "config.h"
#include "updater.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>

static std::mutex g_lock;
static std::condition_variable g_cv;
static std::deque<std::wstring> g_pending;
static std::unordered_map<std::wstring, FolderSize> g_cache;   // lowercase path
static std::unordered_map<std::wstring, DWORD> g_cacheTime;
static HWND g_notify = nullptr;
static bool g_started = false;
static std::atomic<uint64_t> g_epoch{0};
static DWORD g_lastPost = 0;

bool SizesAllowed(const std::wstring& folder) {
    if (!g_config.folderSizes || folder.size() < 3) return false;
    if (folder.rfind(L"\\\\", 0) == 0) return g_config.folderSizesNetwork;
    std::wstring root = folder.substr(0, 3);
    UINT t = GetDriveTypeW(root.c_str());
    if (t == DRIVE_REMOTE) return g_config.folderSizesNetwork;
    return t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_RAMDISK;
}

bool SizesGet(const std::wstring& folder, FolderSize& out) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_cache.find(ToLower(folder));
    if (it == g_cache.end()) return false;
    out = it->second;
    return true;
}

void SizesInvalidate(const std::wstring& path) {
    std::lock_guard<std::mutex> l(g_lock);
    std::wstring p = ToLower(path);
    while (!p.empty()) {
        g_cache.erase(p);
        g_cacheTime.erase(p);
        std::wstring parent = ToLower(PathParent(p));
        if (parent == p) break;
        p = parent;
    }
}

void SizesCancelPending() {
    std::lock_guard<std::mutex> l(g_lock);
    g_pending.clear();
    ++g_epoch;
}

void SizesRequest(const std::vector<std::wstring>& folders, bool force) {
    std::lock_guard<std::mutex> l(g_lock);
    DWORD now = GetTickCount();
    // newest requests first, in the order given
    for (auto it = folders.rbegin(); it != folders.rend(); ++it) {
        std::wstring k = ToLower(*it);
        auto c = g_cache.find(k);
        if (!force && c != g_cache.end() && c->second.done && now - g_cacheTime[k] < 5 * 60 * 1000) continue;
        if (force) { g_cache.erase(k); g_cacheTime.erase(k); }
        g_pending.push_front(*it);
    }
    if (g_pending.size() > 4000) g_pending.resize(4000);
    g_cv.notify_all();
}

static void PostChanged(bool force) {
    DWORD now = GetTickCount();
    if (!force && now - g_lastPost < 250) return;
    g_lastPost = now;
    PostMessageW(g_notify, WM_STRATA_SIZES, 0, 0);
}

static bool Busy() {
    // pause for full-screen games / presentations and when the CPU is pegged
    static FILETIME pi{}, pk{}, pu{};
    FILETIME i, k, u;
    bool heavy = false;
    if (GetSystemTimes(&i, &k, &u)) {
        auto d = [](FILETIME a, FILETIME b) { return (((uint64_t)a.dwHighDateTime << 32) | a.dwLowDateTime) - (((uint64_t)b.dwHighDateTime << 32) | b.dwLowDateTime); };
        uint64_t idle = d(i, pi), total = d(k, pk) + d(u, pu);
        if (pi.dwLowDateTime || pi.dwHighDateTime) heavy = total > 0 && idle * 100 / total < 12;   // >88% busy
        pi = i; pk = k; pu = u;
    }
    return heavy || updater::UserIsBusy();
}

// Returns the folder's size; caches subfolders down to `depth` levels for the "what's taking space" view.
static FolderSize Measure(const std::wstring& dir, int depth, uint64_t epoch, bool& aborted) {
    FolderSize fs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(PathJoin(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (aborted) break;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // don't follow junctions
                std::wstring sub = PathJoin(dir, fd.cFileName);
                FolderSize child;
                bool cached = false;
                {
                    std::lock_guard<std::mutex> l(g_lock);
                    auto it = g_cache.find(ToLower(sub));
                    if (it != g_cache.end() && it->second.done) { child = it->second; cached = true; }
                }
                if (!cached) child = Measure(sub, depth - 1, epoch, aborted);
                fs.bytes += child.bytes;
                fs.files += child.files;
                fs.folders += child.folders + 1;
            } else {
                fs.bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                fs.files++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!aborted && depth >= 0) {
        fs.done = true;
        std::lock_guard<std::mutex> l(g_lock);
        g_cache[ToLower(dir)] = fs;
        g_cacheTime[ToLower(dir)] = GetTickCount();
        if (g_cache.size() > 200000) { g_cache.clear(); g_cacheTime.clear(); }
    }
    fs.done = !aborted;
    return fs;
}

static void Worker() {
    // background mode lowers CPU *and* disk priority
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        std::wstring dir;
        uint64_t epoch;
        {
            std::unique_lock<std::mutex> l(g_lock);
            g_cv.wait(l, [] { return !g_pending.empty(); });
            dir = g_pending.front();
            g_pending.pop_front();
            epoch = g_epoch;
            auto c = g_cache.find(ToLower(dir));
            if (c != g_cache.end() && c->second.done && GetTickCount() - g_cacheTime[ToLower(dir)] < 5 * 60 * 1000) continue;
        }
        while (Busy()) Sleep(2000);
        bool aborted = false;
        Measure(dir, 2, epoch, aborted);
        PostChanged(false);
        bool empty;
        { std::lock_guard<std::mutex> l(g_lock); empty = g_pending.empty(); }
        if (empty) PostChanged(true);
    }
}

void SizesInit(HWND notify) {
    g_notify = notify;
    if (g_started) return;
    g_started = true;
    std::thread(Worker).detach();
}
