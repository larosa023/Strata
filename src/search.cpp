// Strata - search across folders and drives (Everything IPC, or a built-in background scan)
#include "search.h"
#include <shlwapi.h>
#include <thread>
#include <map>
#include <algorithm>

// ============================================================ query matching (built-in scan)
struct Matcher {
    std::vector<std::wstring> words;   // lowercase; every word must match (like Everything)
    bool Init(const std::wstring& q) {
        std::wstring cur;
        for (wchar_t c : ToLower(q)) {
            if (c == L' ' || c == L'\t') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
            else if (c != L'"') cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
        return !words.empty();
    }
    bool Match(const std::wstring& nameLower) const {
        for (auto& w : words) {
            if (w.find_first_of(L"*?") != std::wstring::npos) {
                if (!PathMatchSpecW(nameLower.c_str(), w.c_str())) return false;
            } else if (nameLower.find(w) == std::wstring::npos) {
                return false;
            }
        }
        return true;
    }
};

static std::vector<std::wstring> AllDriveRoots() {
    std::vector<std::wstring> roots;
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW((DWORD)std::size(buf), buf);
    if (!n || n > std::size(buf)) return roots;
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    for (wchar_t* p = buf; *p; p += wcslen(p) + 1) {
        UINT t = GetDriveTypeW(p);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;   // network shares are left out of "everywhere"
        ULARGE_INTEGER a, b, c;
        if (GetDiskFreeSpaceExW(p, &a, &b, &c)) roots.push_back(p);
    }
    SetErrorMode(oldMode);
    return roots;
}

static void ScanThread(HWND notify, int tab, uint64_t gen, std::vector<std::wstring> roots, std::wstring query) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    Matcher m;
    m.Init(query);
    if (roots.empty()) roots = AllDriveRoots();

    auto* batch = new SearchBatch;
    std::map<std::wstring, int> typeIdx;
    size_t found = 0, folders = 0;
    DWORD lastPost = GetTickCount();
    auto post = [&](bool done, bool capped) {
        batch->tab = tab;
        batch->gen = gen;
        batch->done = done;
        batch->capped = capped;
        batch->scannedFolders = folders;
        if (!PostMessageW(notify, WM_STRATA_SEARCH, 0, (LPARAM)batch)) delete batch;
        batch = new SearchBatch;
        typeIdx.clear();
        lastPost = GetTickCount();
    };

    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    std::vector<std::wstring> stack(roots.rbegin(), roots.rend());
    bool cancelled = false, capped = false;
    while (!stack.empty()) {
        std::wstring dir = stack.back();
        stack.pop_back();
        if ((++folders & 63) == 0 && Superseded(tab, gen)) { cancelled = true; break; }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(PathJoin(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        size_t subStart = stack.size();
        do {
            const wchar_t* nm = fd.cFileName;
            if (!wcscmp(nm, L".") || !wcscmp(nm, L"..")) continue;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wstring full = PathJoin(dir, nm);
            if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && nm[0] != L'$' &&
                _wcsicmp(nm, L"System Volume Information") != 0)
                stack.push_back(full);
            if (!m.Match(ToLower(nm))) continue;
            Item it;
            it.name = nm;
            it.path = full;
            it.attrs = fd.dwFileAttributes;
            it.modified = fd.ftLastWriteTime;
            it.size = isDir ? 0 : ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            std::wstring tn;
            TypeAndIcon(it.name, isDir, tn, it.icon);
            auto ti = typeIdx.find(tn);
            if (ti == typeIdx.end()) { ti = typeIdx.emplace(tn, (int)batch->typeNames.size()).first; batch->typeNames.push_back(tn); }
            it.type = ti->second;
            batch->items.push_back(std::move(it));
            if (++found >= kSearchMaxResults) { capped = true; break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        // visit subfolders in name order
        std::reverse(stack.begin() + subStart, stack.end());
        if (capped) break;
        if (GetTickCount() - lastPost > 200) post(false, false);
    }
    SetErrorMode(oldMode);
    if (cancelled) delete batch;
    else post(true, capped);
    CoUninitialize();
}

// ============================================================ Everything IPC
// Message layout from the Everything SDK (Everything_ipc.h), version 2 queries.
#define EVERYTHING_IPC_COPYDATA_QUERY2W 18
#define EVERYTHING_REQUEST_FILE_NAME 0x00000001
#define EVERYTHING_REQUEST_PATH 0x00000002
#define EVERYTHING_REQUEST_FULL_PATH_AND_FILE_NAME 0x00000004
#define EVERYTHING_REQUEST_EXTENSION 0x00000008
#define EVERYTHING_REQUEST_SIZE 0x00000010
#define EVERYTHING_REQUEST_DATE_CREATED 0x00000020
#define EVERYTHING_REQUEST_DATE_MODIFIED 0x00000040
#define EVERYTHING_REQUEST_DATE_ACCESSED 0x00000080
#define EVERYTHING_REQUEST_ATTRIBUTES 0x00000100
#define EVERYTHING_IPC_FOLDER 0x00000001
#define EVERYTHING_SORT_NAME_ASCENDING 1

#pragma pack(push, 1)
struct EvQuery2 {
    DWORD reply_hwnd;
    DWORD reply_copydata_message;
    DWORD search_flags;
    DWORD offset;
    DWORD max_results;
    DWORD request_flags;
    DWORD sort_type;
    // followed by the search string (UTF-16, NUL terminated)
};
struct EvList2 { DWORD totitems, numitems, offset, request_flags, sort_type; };
struct EvItem2 { DWORD flags, data_offset; };
#pragma pack(pop)

static HWND EverythingWindow() {
    HWND w = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
    if (!w) w = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)", nullptr);
    return w;
}

bool EverythingAvailable() { return EverythingWindow() != nullptr; }

static DWORD g_evReplyId = 0;
static int g_evTab = 0;
static uint64_t g_evGen = 0;

static bool EverythingQuery(HWND notify, int tab, uint64_t gen, const std::vector<std::wstring>& roots, const std::wstring& query) {
    HWND ev = EverythingWindow();
    if (!ev) return false;
    std::wstring q;
    if (roots.size() == 1) {
        std::wstring r = roots[0];
        if (r.back() != L'\\') r += L'\\';
        q = L"\"" + r + L"\" ";   // a path ending in \ limits the search to that folder and below
    }
    q += query;
    std::vector<BYTE> buf(sizeof(EvQuery2) + (q.size() + 1) * sizeof(wchar_t));
    auto* hdr = reinterpret_cast<EvQuery2*>(buf.data());
    g_evReplyId = 0x53540000u | (DWORD)(gen & 0xFFFF);
    g_evTab = tab;
    g_evGen = gen;
    hdr->reply_hwnd = (DWORD)(ULONG_PTR)notify;
    hdr->reply_copydata_message = g_evReplyId;
    hdr->search_flags = 0;
    hdr->offset = 0;
    hdr->max_results = (DWORD)kSearchMaxResults;
    hdr->request_flags = EVERYTHING_REQUEST_FILE_NAME | EVERYTHING_REQUEST_PATH | EVERYTHING_REQUEST_SIZE |
                         EVERYTHING_REQUEST_DATE_MODIFIED | EVERYTHING_REQUEST_ATTRIBUTES;
    hdr->sort_type = EVERYTHING_SORT_NAME_ASCENDING;
    memcpy(buf.data() + sizeof(EvQuery2), q.c_str(), (q.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT cds{EVERYTHING_IPC_COPYDATA_QUERY2W, (DWORD)buf.size(), buf.data()};
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(ev, WM_COPYDATA, (WPARAM)notify, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &result) || !result) {
        Log(L"Everything didn't accept the query (error %lu); falling back to a scan", GetLastError());
        g_evReplyId = 0;
        return false;
    }
    return true;
}

bool SearchHandleCopyData(HWND notify, const COPYDATASTRUCT* cds) {
    if (!cds || !g_evReplyId || cds->dwData != g_evReplyId) return false;
    g_evReplyId = 0;
    if (cds->cbData < sizeof(EvList2)) return true;
    const BYTE* base = static_cast<const BYTE*>(cds->lpData);
    const BYTE* end = base + cds->cbData;
    const auto* list = reinterpret_cast<const EvList2*>(base);
    const auto* items = reinterpret_cast<const EvItem2*>(base + sizeof(EvList2));
    if ((const BYTE*)(items + list->numitems) > end) return true;

    auto* batch = new SearchBatch;
    batch->tab = g_evTab;
    batch->gen = g_evGen;
    batch->done = true;
    batch->everything = true;
    batch->total = list->totitems;
    batch->capped = list->totitems > list->numitems;
    std::map<std::wstring, int> typeIdx;
    DWORD rf = list->request_flags;
    for (DWORD i = 0; i < list->numitems; ++i) {
        const BYTE* p = base + items[i].data_offset;
        bool ok = true;
        auto readStr = [&](std::wstring& out) {
            if (p + sizeof(DWORD) > end) { ok = false; return; }
            DWORD len = *reinterpret_cast<const DWORD*>(p);
            p += sizeof(DWORD);
            if (p + (len + 1) * sizeof(wchar_t) > end) { ok = false; return; }
            out.assign(reinterpret_cast<const wchar_t*>(p), len);
            p += (len + 1) * sizeof(wchar_t);
        };
        auto skip = [&](size_t n) { if (p + n > end) ok = false; else p += n; };
        std::wstring name, path, dummy;
        LARGE_INTEGER size{};
        FILETIME modified{};
        DWORD attrs = 0;
        if (rf & EVERYTHING_REQUEST_FILE_NAME) readStr(name);
        if (ok && (rf & EVERYTHING_REQUEST_PATH)) readStr(path);
        if (ok && (rf & EVERYTHING_REQUEST_FULL_PATH_AND_FILE_NAME)) readStr(dummy);
        if (ok && (rf & EVERYTHING_REQUEST_EXTENSION)) readStr(dummy);
        if (ok && (rf & EVERYTHING_REQUEST_SIZE)) { if (p + 8 <= end) size = *reinterpret_cast<const LARGE_INTEGER*>(p); skip(8); }
        if (ok && (rf & EVERYTHING_REQUEST_DATE_CREATED)) skip(8);
        if (ok && (rf & EVERYTHING_REQUEST_DATE_MODIFIED)) { if (p + 8 <= end) modified = *reinterpret_cast<const FILETIME*>(p); skip(8); }
        if (ok && (rf & EVERYTHING_REQUEST_DATE_ACCESSED)) skip(8);
        if (ok && (rf & EVERYTHING_REQUEST_ATTRIBUTES)) { if (p + 4 <= end) attrs = *reinterpret_cast<const DWORD*>(p); skip(4); }
        if (!ok || name.empty()) continue;
        bool isDir = (items[i].flags & EVERYTHING_IPC_FOLDER) != 0;
        if (isDir) attrs |= FILE_ATTRIBUTE_DIRECTORY;
        else attrs &= ~(DWORD)FILE_ATTRIBUTE_DIRECTORY;
        Item it;
        it.name = name;
        it.path = path.empty() ? name : PathJoin(path, name);
        it.attrs = attrs;
        it.modified = modified;
        it.size = (isDir || size.QuadPart < 0) ? 0 : (uint64_t)size.QuadPart;
        std::wstring tn;
        TypeAndIcon(name, isDir, tn, it.icon);
        auto ti = typeIdx.find(tn);
        if (ti == typeIdx.end()) { ti = typeIdx.emplace(tn, (int)batch->typeNames.size()).first; batch->typeNames.push_back(tn); }
        it.type = ti->second;
        batch->items.push_back(std::move(it));
    }
    if (!PostMessageW(notify, WM_STRATA_SEARCH, 0, (LPARAM)batch)) delete batch;
    return true;
}

// ============================================================ entry
void SearchAsync(HWND notify, int tab, uint64_t gen, const std::vector<std::wstring>& roots, const std::wstring& query) {
    MarkLatest(tab, gen);
    if (EverythingQuery(notify, tab, gen, roots, query)) return;
    std::thread(ScanThread, notify, tab, gen, roots, query).detach();
}
