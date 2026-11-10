// Strata - Windows shell integration: icons, context menus, file operations, clipboard
#include "shell.h"
#include "fs.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <commoncontrols.h>
#include <wincodec.h>
#include <thread>
#include <unordered_map>
#include <set>
#include <map>
#include <propkey.h>
#include "config.h"

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ============================================================ icons
static IImageList* g_imageList = nullptr;
static IWICImagingFactory* g_wic = nullptr;
static std::unordered_map<int, ID2D1Bitmap*> g_iconCache;
static ID2D1RenderTarget* g_iconRt = nullptr;
static bool g_iconLarge = false;
// IID_IImageList (not exported by every SDK's import libraries)
static const IID kIID_IImageList = {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};

void IconsInit(bool large) {
    if (g_imageList && large == g_iconLarge) return;
    IconsDiscard();
    Rel(g_imageList);
    g_iconLarge = large;
    SHGetImageList(large ? SHIL_LARGE : SHIL_SMALL, kIID_IImageList, reinterpret_cast<void**>(&g_imageList));
    if (!g_wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
}

void IconsDiscard() {
    for (auto& kv : g_iconCache) if (kv.second) kv.second->Release();
    g_iconCache.clear();
    g_iconRt = nullptr;
}

ID2D1Bitmap* IconBitmap(ID2D1RenderTarget* rt, int index) {
    if (index < 0 || !g_imageList || !g_wic) return nullptr;
    if (rt != g_iconRt) { IconsDiscard(); g_iconRt = rt; }
    auto it = g_iconCache.find(index);
    if (it != g_iconCache.end()) return it->second;
    ID2D1Bitmap* bmp = nullptr;
    HICON icon = nullptr;
    if (SUCCEEDED(g_imageList->GetIcon(index, ILD_TRANSPARENT, &icon)) && icon) {
        IWICBitmap* wb = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(g_wic->CreateBitmapFromHICON(icon, &wb)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeMedianCut))) {
            rt->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
        }
        Rel(conv);
        Rel(wb);
        DestroyIcon(icon);
    }
    g_iconCache[index] = bmp;   // cache failures too
    return bmp;
}

// ============================================================ PIDL helpers
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct PidlSet {
    IShellFolder* parent = nullptr;
    std::vector<PIDLIST_ABSOLUTE> abs;
    std::vector<PCUITEMID_CHILD> children;
    ~PidlSet() {
        Rel(parent);
        for (auto p : abs) CoTaskMemFree(p);
    }
    bool Build(const std::vector<std::wstring>& paths) {
        for (auto& p : paths) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) continue;
            abs.push_back(pidl);
        }
        if (abs.empty()) return false;
        PCUITEMID_CHILD child = nullptr;
        if (FAILED(SHBindToParent(abs[0], IID_PPV_ARGS(&parent), &child))) return false;
        for (auto p : abs) children.push_back(ILFindLastID(p));
        return true;
    }
};
}  // namespace

template <class T>
static T* GetUIObject(HWND owner, const std::vector<std::wstring>& paths, REFIID iid) {
    PidlSet set;
    if (!set.Build(paths)) return nullptr;
    void* out = nullptr;
    if (FAILED(set.parent->GetUIObjectOf(owner, (UINT)set.children.size(), set.children.data(), iid, nullptr, &out)))
        return nullptr;
    return static_cast<T*>(out);
}

// ============================================================ file operations
std::wstring UniqueName(const std::wstring& folder, const std::wstring& base, const std::wstring& ext) {
    std::wstring name = base + ext;
    for (int i = 2; GetFileAttributesW(PathJoin(folder, name).c_str()) != INVALID_FILE_ATTRIBUTES && i < 10000; ++i)
        name = base + L" (" + std::to_wstring(i) + L")" + ext;
    return name;
}

// Fallback for systems where IFileOperation isn't available: the older SHFileOperation.
static HRESULT LegacyFileOp(HWND owner, OpKind kind, const std::vector<std::wstring>& sources, const std::wstring& dest, bool& aborted) {
    std::wstring from;
    for (auto& s : sources) { from += s; from.push_back(L'\0'); }
    from.push_back(L'\0');
    std::wstring to = dest;
    to.push_back(L'\0');
    to.push_back(L'\0');
    SHFILEOPSTRUCTW fo{};
    fo.hwnd = owner;
    fo.wFunc = kind == OpKind::Copy ? FO_COPY : kind == OpKind::Move ? FO_MOVE : FO_DELETE;
    fo.pFrom = from.c_str();
    fo.pTo = (kind == OpKind::Copy || kind == OpKind::Move) ? to.c_str() : nullptr;
    fo.fFlags = FOF_NOCONFIRMMKDIR | (kind == OpKind::Delete ? 0 : FOF_ALLOWUNDO);
    if (kind == OpKind::Copy && !sources.empty() && _wcsicmp(PathParent(sources[0]).c_str(), NormalizeFolder(dest).c_str()) == 0) {
        // duplicating in place: give each copy a "name - Copy" target
        to.clear();
        for (auto& s : sources) {
            std::wstring leaf = PathLeaf(s), base = leaf, ext;
            size_t dot = leaf.find_last_of(L'.');
            if (!IsDirectory(s) && dot != std::wstring::npos && dot > 0) { base = leaf.substr(0, dot); ext = leaf.substr(dot); }
            to += PathJoin(dest, UniqueName(dest, base + L" - Copy", ext));
            to.push_back(L'\0');
        }
        to.push_back(L'\0');
        fo.pTo = to.c_str();
        fo.fFlags |= FOF_MULTIDESTFILES;
    }
    int r = SHFileOperationW(&fo);
    aborted = fo.fAnyOperationsAborted != FALSE;
    return r == 0 ? S_OK : HRESULT_FROM_WIN32(r);
}

static bool NoFileOperation(HRESULT hr) { return hr == E_NOTIMPL || hr == REGDB_E_CLASSNOTREG || hr == E_NOINTERFACE; }

void FileOpAsync(HWND owner, OpKind kind, const std::vector<std::wstring>& sources, const std::wstring& destFolder) {
    std::thread([owner, kind, sources, destFolder] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto* res = new OpResult;
        res->kind = kind;
        res->dest = destFolder;
        res->sources = sources;
        IFileOperation* op = nullptr;
        IShellItem* dest = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
        if (SUCCEEDED(hr)) {
            op->SetOwnerWindow(owner);
            DWORD flags = FOF_NOCONFIRMMKDIR | FOFX_EARLYFAILURE;
            if (kind == OpKind::Recycle) flags |= FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE;
            else if (kind != OpKind::Delete) flags |= FOF_ALLOWUNDO;
            // Copying into the folder the items already live in makes "name - Copy" duplicates, like Explorer.
            if (kind == OpKind::Copy && !sources.empty() &&
                _wcsicmp(PathParent(sources[0]).c_str(), NormalizeFolder(destFolder).c_str()) == 0)
                flags |= FOF_RENAMEONCOLLISION;
            op->SetOperationFlags(flags);
            if (kind == OpKind::Copy || kind == OpKind::Move)
                hr = SHCreateItemFromParsingName(destFolder.c_str(), nullptr, IID_PPV_ARGS(&dest));
            int queued = 0;
            HRESULT lastErr = E_FAIL;
            for (auto& s : sources) {
                if (FAILED(hr)) break;
                IShellItem* item = nullptr;
                if (FAILED(SHCreateItemFromParsingName(s.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                    Log(L"File op: can't resolve %ls", s.c_str());
                    continue;
                }
                HRESULT h2 = E_FAIL;
                if (kind == OpKind::Copy) h2 = op->CopyItem(item, dest, nullptr, nullptr);
                else if (kind == OpKind::Move) h2 = op->MoveItem(item, dest, nullptr, nullptr);
                else h2 = op->DeleteItem(item, nullptr);
                if (SUCCEEDED(h2)) ++queued;
                else lastErr = h2;
                item->Release();
            }
            if (SUCCEEDED(hr) && queued) {
                hr = op->PerformOperations();
                BOOL aborted = FALSE;
                op->GetAnyOperationsAborted(&aborted);
                res->aborted = aborted != FALSE;
            } else if (SUCCEEDED(hr)) {
                hr = lastErr;
            }
        }
        Rel(dest);
        Rel(op);
        if (NoFileOperation(hr)) {
            bool aborted = false;
            hr = LegacyFileOp(owner, kind, sources, destFolder, aborted);
            res->aborted = aborted;
        }
        res->hr = hr;
        res->ok = SUCCEEDED(hr);
        Log(L"File op %d: %u item(s) -> %ls: hr=0x%08lx aborted=%d", (int)kind, (unsigned)sources.size(),
            destFolder.c_str(), hr, res->aborted);
        if (!PostMessageW(owner, WM_STRATA_OPDONE, 0, (LPARAM)res)) delete res;
        CoUninitialize();
    }).detach();
}

bool RenameItem(HWND owner, const std::wstring& path, const std::wstring& newName, std::wstring* error) {
    static const wchar_t* kBad = L"\\/:*?\"<>|";
    if (newName.empty() || newName.find_first_of(kBad) != std::wstring::npos) {
        if (error) *error = L"A name can't be empty or contain \\ / : * ? \" < > |";
        return false;
    }
    IFileOperation* op = nullptr;
    IShellItem* item = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (SUCCEEDED(hr)) {
        op->SetOwnerWindow(owner);
        op->SetOperationFlags(FOF_ALLOWUNDO);
        hr = op->RenameItem(item, newName.c_str(), nullptr);
        if (SUCCEEDED(hr)) hr = op->PerformOperations();
        BOOL aborted = FALSE;
        if (SUCCEEDED(hr)) op->GetAnyOperationsAborted(&aborted);
        if (aborted) hr = E_ABORT;
    }
    Rel(item);
    Rel(op);
    if (NoFileOperation(hr)) {
        std::wstring target = PathJoin(PathParent(path), newName);
        if (_wcsicmp(target.c_str(), path.c_str()) != 0 && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (error) *error = L"There's already an item named \"" + newName + L"\" here.";
            return false;
        }
        hr = MoveFileExW(path.c_str(), target.c_str(), 0) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(hr)) {
        Log(L"Rename %ls -> %ls failed 0x%08lx", path.c_str(), newName.c_str(), hr);
        if (error && hr != E_ABORT && HRESULT_CODE(hr) != ERROR_CANCELLED) *error = L"Couldn't rename that item.";
        return false;
    }
    return true;
}

// ============================================================ clipboard
static UINT DropEffectFormat() {
    static UINT cf = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return cf;
}

// Data object for items that may live in different folders (e.g. search results)
static IDataObject* DataObjectForPaths(HWND owner, const std::vector<std::wstring>& paths) {
    bool sameParent = true;
    for (auto& p : paths) if (_wcsicmp(PathParent(p).c_str(), PathParent(paths[0]).c_str()) != 0) sameParent = false;
    if (sameParent) return GetUIObject<IDataObject>(owner, paths, IID_IDataObject);
    std::vector<PIDLIST_ABSOLUTE> abs;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) abs.push_back(pidl);
    }
    IDataObject* data = nullptr;
    if (!abs.empty())
        SHCreateDataObject(nullptr, (UINT)abs.size(), (PCUITEMID_CHILD_ARRAY)abs.data(), nullptr, IID_PPV_ARGS(&data));
    for (auto p : abs) CoTaskMemFree(p);
    return data;
}

bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths, bool cut) {
    IDataObject* data = DataObjectForPaths(owner, paths);
    if (!data) return false;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (g) {
        *(DWORD*)GlobalLock(g) = cut ? DROPEFFECT_MOVE : (DROPEFFECT_COPY | DROPEFFECT_LINK);
        GlobalUnlock(g);
        FORMATETC fe{(CLIPFORMAT)DropEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        sm.tymed = TYMED_HGLOBAL;
        sm.hGlobal = g;
        if (FAILED(data->SetData(&fe, &sm, TRUE))) GlobalFree(g);
    }
    HRESULT hr = OleSetClipboard(data);
    if (SUCCEEDED(hr)) OleFlushClipboard();
    data->Release();
    return SUCCEEDED(hr);
}

bool ClipboardHasFiles() { return IsClipboardFormatAvailable(CF_HDROP) != FALSE; }

bool ClipboardGetFiles(std::vector<std::wstring>& paths, bool& cut) {
    paths.clear();
    cut = false;
    IDataObject* data = nullptr;
    if (FAILED(OleGetClipboard(&data)) || !data) return false;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (SUCCEEDED(data->GetData(&fe, &sm))) {
        HDROP drop = (HDROP)GlobalLock(sm.hGlobal);
        if (drop) {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring s(len, L'\0');
                DragQueryFileW(drop, i, &s[0], len + 1);
                paths.push_back(s);
            }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
    }
    FORMATETC fe2{(CLIPFORMAT)DropEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm2{};
    if (SUCCEEDED(data->GetData(&fe2, &sm2))) {
        DWORD* eff = (DWORD*)GlobalLock(sm2.hGlobal);
        if (eff) { cut = (*eff & DROPEFFECT_MOVE) != 0; GlobalUnlock(sm2.hGlobal); }
        ReleaseStgMedium(&sm2);
    }
    data->Release();
    return !paths.empty();
}

void ClipboardSetText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        memcpy(GlobalLock(g), text.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

// ============================================================ launching
static bool ResolveShortcutFolder(const std::wstring& lnk, std::wstring& target) {
    IShellLinkW* link = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
        wchar_t buf[MAX_PATH * 4] = {};
        if (SUCCEEDED(link->GetPath(buf, (int)std::size(buf), nullptr, SLGP_RAWPATH)) && buf[0]) {
            wchar_t exp[MAX_PATH * 4];
            ExpandEnvironmentStringsW(buf, exp, (DWORD)std::size(exp));
            if (IsDirectory(exp)) { target = NormalizeFolder(exp); ok = true; }
        }
    }
    Rel(pf);
    Rel(link);
    return ok;
}

bool OpenItem(HWND owner, const std::wstring& path, std::wstring* folderTarget) {
    std::wstring lower = ToLower(path);
    if (folderTarget && lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".lnk") == 0 &&
        ResolveShortcutFolder(path, *folderTarget))
        return false;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
    sei.hwnd = owner;
    sei.lpFile = path.c_str();
    std::wstring dir = PathParent(path);
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) Log(L"Open %ls failed (%lu)", path.c_str(), GetLastError());
    return true;
}

bool CreateShortcutIn(const std::wstring& target, const std::wstring& folder) {
    IShellLinkW* link = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    std::wstring leaf = PathLeaf(target);
    std::wstring lnk = PathJoin(folder, UniqueName(folder, leaf + L" - Shortcut", L".lnk"));
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(PathParent(target).c_str());
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    }
    Rel(pf);
    Rel(link);
    return ok;
}

void OpenTerminal(const std::wstring& folder) {
    if (folder.empty()) return;
    std::wstring args = L"-d \"" + folder + (folder.back() == L'\\' ? L"\\\"" : L"\"");
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"wt.exe", args.c_str(), folder.c_str(), SW_SHOWNORMAL) > 32) return;
    ShellExecuteW(nullptr, L"open", L"cmd.exe", nullptr, folder.c_str(), SW_SHOWNORMAL);
}

static bool InvokeVerb(HWND owner, const std::vector<std::wstring>& paths, const char* verb) {
    IContextMenu* cm = GetUIObject<IContextMenu>(owner, paths, IID_IContextMenu);
    if (!cm) return false;
    HMENU menu = CreatePopupMenu();
    cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
    CMINVOKECOMMANDINFO ci{sizeof(ci)};
    ci.hwnd = owner;
    ci.lpVerb = verb;
    ci.nShow = SW_SHOWNORMAL;
    HRESULT hr = cm->InvokeCommand(&ci);
    DestroyMenu(menu);
    cm->Release();
    return SUCCEEDED(hr);
}

void ShowProperties(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    if (paths.size() == 1) { SHObjectProperties(owner, SHOP_FILEPATH, paths[0].c_str(), nullptr); return; }
    InvokeVerb(owner, paths, "properties");
}

// ============================================================ recycle bin restore
size_t RestoreFromRecycleBin(HWND owner, const std::vector<std::wstring>& originalPaths) {
    if (originalPaths.empty()) return 0;
    std::set<std::wstring> want;
    for (auto& p : originalPaths) want.insert(ToLower(p));
    PIDLIST_ABSOLUTE binPidl = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &binPidl))) return 0;
    IShellFolder* desktop = nullptr;
    IShellFolder2* bin = nullptr;
    SHGetDesktopFolder(&desktop);
    if (desktop) desktop->BindToObject(binPidl, nullptr, IID_PPV_ARGS(&bin));
    size_t restored = 0;
    if (bin) {
        // newest deletion per original path wins
        struct Hit { PITEMID_CHILD child; double when; };
        std::map<std::wstring, Hit> hits;
        IEnumIDList* en = nullptr;
        if (SUCCEEDED(bin->EnumObjects(owner, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en)) && en) {
            PITEMID_CHILD child = nullptr;
            const SHCOLUMNID kFrom{{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 2};
            const SHCOLUMNID kWhen{{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 3};
            while (en->Next(1, &child, nullptr) == S_OK) {
                VARIANT from, when;
                VariantInit(&from);
                VariantInit(&when);
                std::wstring folderFrom, name;
                if (SUCCEEDED(bin->GetDetailsEx(child, &kFrom, &from)) && from.vt == VT_BSTR) folderFrom = from.bstrVal;
                STRRET sr;
                if (SUCCEEDED(bin->GetDisplayNameOf(child, SHGDN_INFOLDER, &sr))) {
                    wchar_t* nm = nullptr;
                    if (SUCCEEDED(StrRetToStrW(&sr, child, &nm))) { name = nm; CoTaskMemFree(nm); }
                }
                double t = 0;
                if (SUCCEEDED(bin->GetDetailsEx(child, &kWhen, &when)) && when.vt == VT_DATE) t = when.date;
                VariantClear(&from);
                VariantClear(&when);
                std::wstring key = ToLower(PathJoin(folderFrom, name));
                bool keep = false;
                if (want.count(key)) {
                    auto it = hits.find(key);
                    if (it == hits.end() || it->second.when < t) {
                        if (it != hits.end()) CoTaskMemFree(it->second.child);
                        hits[key] = {child, t};
                        keep = true;
                    }
                }
                // Recycle Bin display names may hide extensions; also try matching without them
                if (!keep && !name.empty()) {
                    for (auto& w : want) {
                        if (ToLower(PathParent(w)) != ToLower(folderFrom)) continue;
                        std::wstring leaf = PathLeaf(w);
                        size_t dot = leaf.find_last_of(L'.');
                        if (dot != std::wstring::npos && _wcsicmp(leaf.substr(0, dot).c_str(), name.c_str()) == 0) {
                            auto it = hits.find(w);
                            if (it == hits.end() || it->second.when < t) {
                                if (it != hits.end()) CoTaskMemFree(it->second.child);
                                hits[w] = {child, t};
                                keep = true;
                            }
                            break;
                        }
                    }
                }
                if (!keep) CoTaskMemFree(child);
            }
            en->Release();
        }
        if (!hits.empty()) {
            std::vector<PCUITEMID_CHILD> kids;
            for (auto& h : hits) kids.push_back(h.second.child);
            IContextMenu* cm = nullptr;
            if (SUCCEEDED(bin->GetUIObjectOf(owner, (UINT)kids.size(), kids.data(), IID_IContextMenu, nullptr, (void**)&cm)) && cm) {
                HMENU m = CreatePopupMenu();
                cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL);
                CMINVOKECOMMANDINFO ci{sizeof(ci)};
                ci.hwnd = owner;
                ci.lpVerb = "undelete";
                ci.nShow = SW_SHOWNORMAL;
                if (SUCCEEDED(cm->InvokeCommand(&ci))) restored = kids.size();
                DestroyMenu(m);
                cm->Release();
            }
            for (auto& h : hits) CoTaskMemFree(h.second.child);
        }
        bin->Release();
    }
    if (desktop) desktop->Release();
    CoTaskMemFree(binPidl);
    return restored;
}

bool ExtractToTemp(HWND owner, const std::wstring& path, std::wstring& out) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"Strata\\" + std::to_wstring(GetTickCount());
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    IFileOperation* op = nullptr;
    IShellItem *src = nullptr, *dst = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&src));
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&dst));
    if (SUCCEEDED(hr)) {
        op->SetOwnerWindow(owner);
        op->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_SILENT);
        hr = op->CopyItem(src, dst, nullptr, nullptr);
        if (SUCCEEDED(hr)) hr = op->PerformOperations();
    }
    Rel(src);
    Rel(dst);
    Rel(op);
    out = PathJoin(dir, PathLeaf(path));
    return SUCCEEDED(hr) && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void ShellCopyOut(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destFolder) {
    FileOpAsync(owner, OpKind::Copy, sources, destFolder);
}

// ============================================================ context menu
static IContextMenu2* g_cm2 = nullptr;
static IContextMenu3* g_cm3 = nullptr;

bool ShellMenuHandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (msg != WM_INITMENUPOPUP && msg != WM_DRAWITEM && msg != WM_MEASUREITEM && msg != WM_MENUCHAR) return false;
    if (g_cm3) {
        LRESULT r = 0;
        if (SUCCEEDED(g_cm3->HandleMenuMsg2(msg, wp, lp, &r))) { *result = r; return true; }
    } else if (g_cm2) {
        if (SUCCEEDED(g_cm2->HandleMenuMsg(msg, wp, lp))) { *result = msg == WM_INITMENUPOPUP ? 0 : TRUE; return true; }
    }
    return false;
}

constexpr UINT kShellFirst = 1, kShellLast = 0x6FFF, kExtraBase = 0x7000;

static std::wstring CleanLabel(const wchar_t* raw) {
    std::wstring out;
    for (const wchar_t* p = raw; *p; ++p) {
        if (*p == L'\t') break;
        if (*p == L'&') { if (p[1] == L'&') { out += L'&'; ++p; } continue; }
        out += *p;
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.' || out.back() == 0x2026)) out.pop_back();
    return out;
}

void ContextMenuSeen(std::vector<std::wstring>& out) { out = g_config.seenMenuItems; }

// Removes Windows menu entries the user chose to hide, and remembers the entries seen (for Settings).
static void CleanShellMenu(HMENU menu, UINT firstShellPos) {
    std::set<std::wstring> hidden;
    for (auto& h : g_config.hiddenMenuItems) hidden.insert(ToLower(h));
    bool seenChanged = false;
    for (int i = GetMenuItemCount(menu) - 1; i >= (int)firstShellPos; --i) {
        wchar_t buf[256] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_ID;
        mi.dwTypeData = buf;
        mi.cch = 255;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mi)) continue;
        if (mi.fType & (MFT_SEPARATOR | MFT_OWNERDRAW)) {
            if (!(mi.fType & MFT_SEPARATOR) || !buf[0]) {}
            if (mi.fType & MFT_SEPARATOR) continue;
        }
        std::wstring label = CleanLabel(buf);
        if (label.empty()) continue;
        if (hidden.count(ToLower(label))) { DeleteMenu(menu, i, MF_BYPOSITION); continue; }
        bool known = false;
        for (auto& s : g_config.seenMenuItems) if (_wcsicmp(s.c_str(), label.c_str()) == 0) { known = true; break; }
        if (!known && mi.wID < 0x7000) {
            g_config.seenMenuItems.push_back(label);
            if (g_config.seenMenuItems.size() > 150) g_config.seenMenuItems.erase(g_config.seenMenuItems.begin());
            seenChanged = true;
        }
    }
    // tidy separators left behind
    bool prevSep = true;
    for (int i = 0; i < GetMenuItemCount(menu);) {
        bool sep = (GetMenuState(menu, i, MF_BYPOSITION) & MF_SEPARATOR) != 0;
        if (sep && prevSep) { DeleteMenu(menu, i, MF_BYPOSITION); continue; }
        prevSep = sep;
        ++i;
    }
    int n = GetMenuItemCount(menu);
    if (n > 0 && (GetMenuState(menu, n - 1, MF_BYPOSITION) & MF_SEPARATOR)) DeleteMenu(menu, n - 1, MF_BYPOSITION);
    if (seenChanged) ConfigSave();
}

int ShowItemContextMenu(HWND owner, POINT pt, const std::wstring& folder, const std::vector<std::wstring>& paths,
                        const std::vector<MenuEntry>& extra,
                        const std::function<bool(const std::wstring& verb)>& onVerb,
                        HMENU extraSubmenu, const wchar_t* extraSubmenuLabel) {
    HMENU menu = CreatePopupMenu();
    UINT pos = 0;
    for (auto& e : extra) {
        if (e.id == 0) InsertMenuW(menu, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        else InsertMenuW(menu, pos++, MF_BYPOSITION | MF_STRING | (e.checked ? MF_CHECKED : 0) | (e.disabled ? MF_GRAYED : 0),
                         kExtraBase + e.id, e.label.c_str());
    }
    if (extraSubmenu) InsertMenuW(menu, pos++, MF_BYPOSITION | MF_POPUP, (UINT_PTR)extraSubmenu, extraSubmenuLabel);
    IContextMenu* cm = paths.empty() ? nullptr : GetUIObject<IContextMenu>(owner, paths, IID_IContextMenu);
    if (cm) {
        if (pos) InsertMenuW(menu, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        UINT flags = CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME;
        if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
        cm->QueryContextMenu(menu, pos, kShellFirst, kShellLast, flags);
        cm->QueryInterface(IID_PPV_ARGS(&g_cm3));
        if (!g_cm3) cm->QueryInterface(IID_PPV_ARGS(&g_cm2));
        if (GetKeyState(VK_SHIFT) >= 0) CleanShellMenu(menu, pos);   // Shift+right-click shows everything
    }
    // Remove a leading/trailing separator the shell may leave
    int count = GetMenuItemCount(menu);
    if (count > 0 && (GetMenuState(menu, count - 1, MF_BYPOSITION) & MF_SEPARATOR)) DeleteMenu(menu, count - 1, MF_BYPOSITION);

    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
    Rel(g_cm3);
    Rel(g_cm2);
    int result = 0;
    if (cmd >= kExtraBase) {
        result = (int)(cmd - kExtraBase);
    } else if (cmd >= kShellFirst && cm) {
        wchar_t verb[128] = {};
        if (FAILED(cm->GetCommandString(cmd - kShellFirst, GCS_VERBW, nullptr, (LPSTR)verb, (UINT)std::size(verb)))) verb[0] = 0;
        if (onVerb && verb[0] && onVerb(ToLower(verb))) {
            result = -1;
        } else {
            CMINVOKECOMMANDINFOEX ci{sizeof(ci)};
            ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            if (GetKeyState(VK_CONTROL) < 0) ci.fMask |= CMIC_MASK_CONTROL_DOWN;
            if (GetKeyState(VK_SHIFT) < 0) ci.fMask |= CMIC_MASK_SHIFT_DOWN;
            ci.hwnd = owner;
            ci.lpVerb = MAKEINTRESOURCEA(cmd - kShellFirst);
            ci.lpVerbW = MAKEINTRESOURCEW(cmd - kShellFirst);
            ci.lpDirectoryW = folder.empty() ? nullptr : folder.c_str();
            ci.nShow = SW_SHOWNORMAL;
            ci.ptInvoke = pt;
            HRESULT hr = cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci);
            if (FAILED(hr)) Log(L"Context menu command %u (%ls) failed 0x%08lx", cmd, verb, hr);
        }
    }
    Rel(cm);
    DestroyMenu(menu);
    return result;
}
