// Strata - drag and drop
#include "dragdrop.h"
#include "fs.h"
#include <shlobj.h>
#include <shellapi.h>

static bool g_draggingOut = false;
static std::vector<std::wstring> g_dragPaths;   // what we're dragging (to tell a no-op drop)
bool DragInProgressFromUs() { return g_draggingOut; }

static bool FilesFromData(IDataObject* data, std::vector<std::wstring>& out) {
    out.clear();
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(data->GetData(&fe, &sm))) return false;
    HDROP drop = (HDROP)GlobalLock(sm.hGlobal);
    if (drop) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(len, L'\0');
            DragQueryFileW(drop, i, &s[0], len + 1);
            out.push_back(s);
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return !out.empty();
}

static std::wstring VolumeOf(const std::wstring& path) {
    wchar_t vol[MAX_PATH] = {};
    if (!GetVolumePathNameW(path.c_str(), vol, MAX_PATH)) return L"";
    return ToLower(vol);
}

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
class DropTarget final : public IDropTarget {
public:
    DropTarget(HWND hwnd, const DropHandlers& h) : hwnd_(hwnd), h_(h) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }
    ~DropTarget() { if (helper_) helper_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        files_.clear();
        hasFiles_ = FilesFromData(data, files_);
        if (helper_) { POINT p{pt.x, pt.y}; helper_->DragEnter(hwnd_, data, &p, *effect); }
        Compute(keys, pt, effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        if (helper_) { POINT p{pt.x, pt.y}; helper_->DragOver(&p, *effect); }
        Compute(keys, pt, effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        if (h_.hover) h_.hover(L"");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        if (helper_) { POINT p{pt.x, pt.y}; helper_->Drop(data, &p, *effect); }
        Compute(keys, pt, effect);
        if (h_.hover) h_.hover(L"");
        if (*effect != DROPEFFECT_NONE && !folder_.empty() && h_.drop) h_.drop(files_, folder_, *effect);
        return S_OK;
    }

private:
    void Compute(DWORD keys, POINTL pt, DWORD* effect) {
        POINT cp{pt.x, pt.y};
        ScreenToClient(hwnd_, &cp);
        folder_ = hasFiles_ && h_.targetAt ? h_.targetAt(cp) : L"";
        if (h_.hover) h_.hover(folder_);
        DWORD allowed = *effect;
        if (folder_.empty()) { *effect = DROPEFFECT_NONE; return; }
        // dropping items onto the folder they're already in, or a folder onto itself, does nothing
        for (auto& f : files_) {
            if (_wcsicmp(f.c_str(), folder_.c_str()) == 0) { *effect = DROPEFFECT_NONE; return; }
        }
        bool sameParent = true;
        for (auto& f : files_) if (_wcsicmp(PathParent(f).c_str(), folder_.c_str()) != 0) sameParent = false;
        DWORD want;
        if ((keys & MK_CONTROL) && (keys & MK_SHIFT)) want = DROPEFFECT_LINK;
        else if (keys & MK_CONTROL) want = DROPEFFECT_COPY;
        else if (keys & MK_SHIFT) want = DROPEFFECT_MOVE;
        else if (keys & MK_ALT) want = DROPEFFECT_LINK;
        else want = (!files_.empty() && VolumeOf(files_[0]) == VolumeOf(folder_)) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;   // like Explorer
        if (sameParent && want != DROPEFFECT_COPY) { *effect = DROPEFFECT_NONE; return; }
        if (!(allowed & want)) want = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        *effect = want;
    }

    LONG ref_ = 1;
    HWND hwnd_;
    DropHandlers h_;
    IDropTargetHelper* helper_ = nullptr;
    std::vector<std::wstring> files_;
    bool hasFiles_ = false;
    std::wstring folder_;
};
}  // namespace

bool DragDropRegister(HWND hwnd, const DropHandlers& handlers) {
    auto* t = new DropTarget(hwnd, handlers);
    HRESULT hr = RegisterDragDrop(hwnd, t);
    t->Release();
    if (FAILED(hr)) Log(L"RegisterDragDrop failed 0x%08lx", hr);
    return SUCCEEDED(hr);
}

void DragDropRevoke(HWND hwnd) { RevokeDragDrop(hwnd); }

DWORD DragFiles(HWND hwnd, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return DROPEFFECT_NONE;
    // a shell data object gives Explorer-quality drags (drag image, works with every app)
    std::vector<PIDLIST_ABSOLUTE> abs;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) abs.push_back(pidl);
    }
    DWORD effect = DROPEFFECT_NONE;
    IDataObject* data = nullptr;
    bool sameParent = true;
    for (auto& p : paths) if (_wcsicmp(PathParent(p).c_str(), PathParent(paths[0]).c_str()) != 0) sameParent = false;
    if (!abs.empty()) {
        if (sameParent) {
            IShellFolder* parent = nullptr;
            PCUITEMID_CHILD child = nullptr;
            if (SUCCEEDED(SHBindToParent(abs[0], IID_PPV_ARGS(&parent), &child))) {
                std::vector<PCUITEMID_CHILD> kids;
                for (auto a : abs) kids.push_back(ILFindLastID(a));
                parent->GetUIObjectOf(hwnd, (UINT)kids.size(), kids.data(), IID_IDataObject, nullptr, (void**)&data);
                parent->Release();
            }
        }
        if (!data) SHCreateDataObject(nullptr, (UINT)abs.size(), (PCUITEMID_CHILD_ARRAY)abs.data(), nullptr, IID_PPV_ARGS(&data));
    }
    if (data) {
        g_draggingOut = true;
        g_dragPaths = paths;
        SHDoDragDrop(hwnd, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        g_draggingOut = false;
        data->Release();
    }
    for (auto p : abs) CoTaskMemFree(p);
    return effect;
}
