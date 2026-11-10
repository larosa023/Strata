// Strata - main window: title bar with tabs, toolbar, breadcrumb, sidebar, file list, status bar
#include "window.h"
#include "config.h"
#include "gfx.h"
#include "fs.h"
#include "shell.h"
#include "textinput.h"
#include "search.h"
#include <uxtheme.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <dbt.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace gfx;

// ============================================================ state
struct Tab {
    int id = 0;
    std::wstring path;                       // "" = This PC
    std::vector<std::wstring> back, fwd;
    std::vector<Item> items;
    std::vector<std::wstring> types;
    std::vector<int> view;                   // indices into items, filtered + sorted
    std::vector<uint8_t> sel;                // per item
    int focus = -1;                          // view position
    int anchor = -1;
    float scroll = 0, scrollTarget = 0;
    bool loading = false, loaded = false;
    DWORD error = 0;
    double ms = 0;
    DWORD loadedAt = 0;
    TextInput filter;
    uint64_t gen = 0;
    // restore after a reload / navigation
    std::set<std::wstring> restoreSel;
    std::wstring restoreFocus;
    bool restoreScroll = false;
    std::wstring selectAfterLoad;            // select (and optionally rename) this name once listed
    bool renameAfterLoad = false;
    // search (This PC: every drive; a folder: it and its subfolders)
    bool searchActive = false, searchRunning = false, searchEverything = false, searchCapped = false;
    std::wstring searchQuery;
    uint64_t searchTotal = 0;
    size_t searchScanned = 0;
};

enum class Focus { List, Search, Path, Rename };

enum ZoneKind {
    ZNone, ZTab, ZTabClose, ZNewTab, ZCapMin, ZCapMax, ZCapClose, ZBack, ZFwd, ZUp, ZRefresh, ZPathBox, ZCrumb, ZCrumbArrow,
    ZCrumbOverflow, ZSearch, ZSearchClear, ZMenu, ZSideThisPC, ZSidePin, ZSideDrive, ZSideSplitter, ZHeader, ZHeaderDiv,
    ZList, ZScrollbar
};
struct Zone { ZoneKind kind; int idx; D2D1_RECT_F rc; };

static HWND g_hwnd;
static UINT g_dpi = 96;
static float g_scale = 1.0f;
static Theme g_theme;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_br;
static Canvas g_cv;
static IDWriteTextFormat *g_fUi, *g_fUiSemi, *g_fSmall, *g_fSmallRight, *g_fRight, *g_fHeader, *g_fIcon, *g_fIconSmall,
    *g_fIconBig, *g_fInput, *g_fTitle, *g_fSection, *g_fEmpty, *g_fCenter;

static std::vector<Tab> g_tabs;
static int g_active = 0;
static int g_nextTabId = 1;
static uint64_t g_nextGen = 1;
static std::vector<std::wstring> g_closedTabs;
static std::vector<DriveInfo> g_drives;
static std::map<std::wstring, std::wstring> g_driveNames;   // "C:\" -> "Local Disk (C:)"

static std::vector<Zone> g_zones;
static ZoneKind g_hotKind = ZNone;
static int g_hotIdx = -1;
static int g_hotRow = -1;
static bool g_tracking = false;
static Focus g_focus = Focus::List;
static TextInput g_pathInput;
static TextInput g_renameInput;
static int g_renameItem = -1;                // item index in the active tab
static bool g_caretTimer = false;

// mouse drags
enum class Drag { None, Tab, Splitter, Column, Scrollbar, Marquee, InputSelect, ListPress };
static Drag g_drag = Drag::None;
static int g_dragIdx = -1;
static float g_dragStartX = 0, g_dragStartY = 0, g_dragOrig = 0;
static bool g_dragMoved = false;
static D2D1_POINT_2F g_marqueeA{}, g_marqueeB{};   // content coordinates (y includes scroll)
static std::vector<uint8_t> g_marqueeBase;
static TextInput* g_dragInput = nullptr;
static D2D1_RECT_F g_dragInputBox{};
static int g_pressRow = -1;
static ZoneKind g_pressKind = ZNone;   // zone a button press started on

// type-ahead
static std::wstring g_typeBuf;
static DWORD g_typeTime = 0;

// transient status message
static std::wstring g_statusMsg;

// sidebar scroll
static float g_sideScroll = 0, g_sideContentH = 0;

// layout rects (DIPs), recomputed each paint
static D2D1_RECT_F g_rcList, g_rcHeader, g_rcSidebar, g_rcPathBox, g_rcSearchBox, g_rcRenameBox;
static float g_W = 0, g_H = 0;

// timers
constexpr UINT_PTR TIMER_SCROLL = 1, TIMER_CARET = 2, TIMER_REFRESH = 3, TIMER_STATUS = 4, TIMER_DRIVES = 5,
                   TIMER_MARQUEE = 6, TIMER_SEARCH = 7;

// metrics (DIPs)
constexpr float kTitleH = 40, kToolH = 48, kStatusH = 28, kHeaderH = 32, kCapW = 46, kSplitW = 6, kResize = 6;
static float RowH() { return g_config.density == L"compact" ? 26.0f : 32.0f; }

static Tab& T() { return g_tabs[g_active]; }
// The This PC page lists drives (unless it's showing search results)
static bool DriveView(const Tab& t) { return t.path.empty() && !t.searchActive; }
static float Px2Dip(int v) { return v / g_scale; }
static void Redraw() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }

static void SetStatus(const std::wstring& msg) {
    g_statusMsg = msg;
    SetTimer(g_hwnd, TIMER_STATUS, 5000, nullptr);
    Redraw();
}

static std::wstring ErrorText(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                   (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"Unknown error";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

static std::wstring WithCommas(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L",");
    return s;
}

// ============================================================ device resources
static void ReleaseFormats() {
    IDWriteTextFormat** all[] = {&g_fUi, &g_fUiSemi, &g_fSmall, &g_fSmallRight, &g_fRight, &g_fHeader, &g_fIcon, &g_fIconSmall,
                                 &g_fIconBig, &g_fInput, &g_fTitle, &g_fSection, &g_fEmpty, &g_fCenter};
    for (auto p : all) SafeRelease(*p);
}

static void CreateFormats() {
    ReleaseFormats();
    const wchar_t* ui = UiFont();
    g_fUi = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fUiSemi = Format(ui, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fSmall = Format(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fSmallRight = Format(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    g_fRight = Format(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    g_fHeader = Format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fIcon = Format(IconFont(), 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconSmall = Format(IconFont(), 10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconBig = Format(IconFont(), 36, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fInput = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fTitle = Format(DisplayFont(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fSection = Format(ui, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fEmpty = Format(DisplayFont(), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_fCenter = Format(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    if (g_fSection) g_fSection->SetIncrementalTabStop(40);
}

static void DiscardDevice() {
    IconsDiscard();
    SafeRelease(g_br);
    SafeRelease(g_rt);
    g_cv.rt = nullptr;
    g_cv.br = nullptr;
}

static bool EnsureDevice() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT hr = d2d->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)),
                                                D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        &g_rt);
    if (FAILED(hr)) { Log(L"CreateHwndRenderTarget failed 0x%08lx", hr); return false; }
    g_rt->SetDpi((float)g_dpi, (float)g_dpi);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &g_br);
    g_cv.rt = g_rt;
    g_cv.br = g_br;
    return true;
}

static void ApplyTheme() {
    g_theme = BuildTheme(g_config);
    ApplyWindowTheme(g_hwnd, g_theme, false);
    if (!g_theme.backdrop) {
        MARGINS m{0, 0, 1, 0};   // keeps the DWM shadow with our custom frame
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
    }
    ApplyMenuTheme(g_theme.dark);
    // Our title bar is custom-drawn; stop Windows painting its own caption, icon and min/max/close
    // buttons into the extended (Mica/acrylic) frame, which showed through as "ghost" buttons.
    WTA_OPTIONS wta{WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON | WTNCA_NOSYSMENU, WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON | WTNCA_NOSYSMENU};
    SetWindowThemeAttribute(g_hwnd, WTA_NONCLIENT, &wta, sizeof(wta));
    Redraw();
}

// ============================================================ view model
static bool ItemHidden(const Item& it) {
    return !g_config.showHidden && (it.attrs & FILE_ATTRIBUTE_HIDDEN) && it.path.empty();
}

static std::wstring DisplayName(const Item& it) {
    if (g_config.showExtensions || it.isDir() || !it.path.empty()) return it.name;
    size_t dot = it.name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return it.name;
    return it.name.substr(0, dot);
}

static std::wstring ItemPath(const Tab& t, const Item& it) {
    if (!it.path.empty()) return it.path;
    return PathJoin(t.path, it.name);
}

static bool MatchesFilter(const std::wstring& nameLower, const std::wstring& filterLower) {
    if (filterLower.empty()) return true;
    if (filterLower.find_first_of(L"*?") != std::wstring::npos) return PathMatchSpecW(nameLower.c_str(), filterLower.c_str()) != FALSE;
    return nameLower.find(filterLower) != std::wstring::npos;
}

static void BuildView(Tab& t) {
    // remember selection by item index -> keep it (items don't change here)
    std::wstring focusName = (t.focus >= 0 && t.focus < (int)t.view.size()) ? t.items[t.view[t.focus]].name : L"";
    t.view.clear();
    std::wstring f = ToLower(t.filter.text);
    while (!f.empty() && f.back() == L' ') f.pop_back();
    for (int i = 0; i < (int)t.items.size(); ++i) {
        const Item& it = t.items[i];
        if (ItemHidden(it)) continue;
        if (!t.searchActive && !f.empty() && !MatchesFilter(ToLower(it.name), f)) continue;
        t.view.push_back(i);
    }
    int col = g_config.sortColumn;
    bool desc = g_config.sortDescending;
    bool thisPC = DriveView(t);
    bool search = t.searchActive;
    auto& items = t.items;
    auto& types = t.types;
    std::stable_sort(t.view.begin(), t.view.end(), [&](int ia, int ib) {
        const Item& a = items[ia];
        const Item& b = items[ib];
        if (thisPC) {
            int c = 0;
            if (col == ColSize) c = a.total < b.total ? -1 : a.total > b.total ? 1 : 0;
            if (!c) c = _wcsicmp(a.path.c_str(), b.path.c_str());
            return desc ? c > 0 : c < 0;
        }
        if (g_config.foldersFirst && a.isDir() != b.isDir()) return a.isDir();
        int c = 0;
        switch (col) {
            case ColSize: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
            case ColModified: c = CompareFileTime(&a.modified, &b.modified); break;
            case ColType:
                c = search ? StrCmpLogicalW(a.path.c_str(), b.path.c_str()) : StrCmpLogicalW(types[a.type].c_str(), types[b.type].c_str());
                break;
            default: break;
        }
        if (!c) c = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
        return desc ? c > 0 : c < 0;
    });
    if (t.sel.size() != t.items.size()) t.sel.assign(t.items.size(), 0);
    // deselect items that are no longer visible
    std::vector<uint8_t> visible(t.items.size(), 0);
    for (int i : t.view) visible[i] = 1;
    for (size_t i = 0; i < t.sel.size(); ++i) if (!visible[i]) t.sel[i] = 0;
    t.focus = -1;
    if (!focusName.empty())
        for (int v = 0; v < (int)t.view.size(); ++v) if (t.items[t.view[v]].name == focusName) { t.focus = v; break; }
    if (t.anchor >= (int)t.view.size()) t.anchor = -1;
}

static float ListViewH() { return std::max(0.0f, g_rcList.bottom - g_rcList.top); }
static float MaxScroll(const Tab& t) { return std::max(0.0f, t.view.size() * RowH() - ListViewH() + 8); }

static void ClampScroll(Tab& t) {
    float m = MaxScroll(t);
    t.scrollTarget = std::clamp(t.scrollTarget, 0.0f, m);
    t.scroll = std::clamp(t.scroll, 0.0f, m);
}

static void EnsureVisible(Tab& t, int v) {
    if (v < 0) return;
    float rh = RowH(), y = v * rh, h = ListViewH();
    if (y < t.scrollTarget) t.scrollTarget = y;
    else if (y + rh > t.scrollTarget + h) t.scrollTarget = y + rh - h + 4;
    ClampScroll(t);
    t.scroll = t.scrollTarget;
}

static std::vector<std::wstring> SelectedPaths(const Tab& t) {
    std::vector<std::wstring> out;
    for (int v : t.view) if (t.sel[v]) out.push_back(ItemPath(t, t.items[v]));
    return out;
}

static int SelectedCount(const Tab& t) {
    int n = 0;
    for (int v : t.view) n += t.sel[v] ? 1 : 0;
    return n;
}

static void SelectOnly(Tab& t, int v) {
    std::fill(t.sel.begin(), t.sel.end(), 0);
    if (v >= 0 && v < (int)t.view.size()) t.sel[t.view[v]] = 1;
    t.focus = v;
    t.anchor = v;
}

static void SelectRange(Tab& t, int a, int b, bool add) {
    if (!add) std::fill(t.sel.begin(), t.sel.end(), 0);
    if (a > b) std::swap(a, b);
    for (int v = std::max(0, a); v <= b && v < (int)t.view.size(); ++v) t.sel[t.view[v]] = 1;
}

// ============================================================ navigation
static void WatchActive() { WatchFolder(g_hwnd, g_tabs.empty() ? L"" : T().path); }

static void StartSearch(Tab& t, const std::wstring& query);

static void Load(Tab& t, bool keepState) {
    if (t.searchActive) { StartSearch(t, t.searchQuery); return; }
    t.gen = g_nextGen++;
    t.loading = true;
    if (keepState) {
        // (restoreSel may already hold names to select, e.g. items just pasted)
        for (int v : t.view) if (t.sel[v]) t.restoreSel.insert(t.items[v].name);
        t.restoreFocus = (t.focus >= 0 && t.focus < (int)t.view.size()) ? t.items[t.view[t.focus]].name : L"";
        t.restoreScroll = true;
    } else {
        t.restoreSel.clear();
        t.restoreFocus.clear();
        t.restoreScroll = false;
    }
    ListFolderAsync(g_hwnd, t.id, t.gen, t.path);
}

static void Navigate(Tab& t, const std::wstring& path, bool addHistory = true, const std::wstring& selectName = L"") {
    std::wstring p = NormalizeFolder(path);
    if (addHistory && _wcsicmp(p.c_str(), t.path.c_str()) != 0) {
        t.back.push_back(t.path);
        if (t.back.size() > 100) t.back.erase(t.back.begin());
        t.fwd.clear();
    }
    t.path = p;
    t.items.clear();
    t.types.clear();
    t.view.clear();
    t.sel.clear();
    t.focus = t.anchor = -1;
    t.scroll = t.scrollTarget = 0;
    t.error = 0;
    t.loaded = false;
    t.filter.SetText(L"");
    t.searchActive = t.searchRunning = false;
    t.searchQuery.clear();
    if (&t == &T()) KillTimer(g_hwnd, TIMER_SEARCH);
    t.selectAfterLoad = selectName;
    t.renameAfterLoad = false;
    Load(t, false);
    if (&t == &T()) {
        WatchActive();
        if (g_focus == Focus::Search || g_focus == Focus::Path || g_focus == Focus::Rename) g_focus = Focus::List;
    }
    Redraw();
}

static void StartSearch(Tab& t, const std::wstring& query) {
    std::wstring q = query;
    while (!q.empty() && q.back() == L' ') q.pop_back();
    while (!q.empty() && q.front() == L' ') q.erase(q.begin());
    if (q.empty()) return;
    t.gen = g_nextGen++;
    t.searchActive = t.searchRunning = true;
    t.searchEverything = t.searchCapped = false;
    t.searchQuery = q;
    t.searchTotal = 0;
    t.searchScanned = 0;
    t.items.clear();
    t.types.clear();
    t.view.clear();
    t.sel.clear();
    t.focus = t.anchor = -1;
    t.scroll = t.scrollTarget = 0;
    t.error = 0;
    t.loading = false;
    t.loaded = true;
    std::vector<std::wstring> roots;
    if (!t.path.empty()) roots.push_back(t.path);
    SearchAsync(g_hwnd, t.id, t.gen, roots, q);
    Redraw();
}

static void EndSearch(Tab& t) {
    KillTimer(g_hwnd, TIMER_SEARCH);
    if (!t.searchActive) return;
    t.searchActive = t.searchRunning = false;
    t.searchQuery.clear();
    t.items.clear();
    t.view.clear();
    t.sel.clear();
    t.loaded = false;
    Load(t, false);
}

// When going back/up to a parent, select the folder we came from.
static std::wstring ChildMarker(const std::wstring& parent, const std::wstring& child) {
    if (child.empty() || _wcsicmp(PathParent(child).c_str(), parent.c_str()) != 0) return L"";
    return parent.empty() ? L"\x01" + child : PathLeaf(child);
}

static void GoBack(Tab& t) {
    if (t.back.empty()) return;
    std::wstring from = t.path;
    std::wstring to = t.back.back();
    t.back.pop_back();
    t.fwd.push_back(from);
    Navigate(t, to, false, ChildMarker(NormalizeFolder(to), from));
}

static void GoForward(Tab& t) {
    if (t.fwd.empty()) return;
    std::wstring to = t.fwd.back();
    t.fwd.pop_back();
    t.back.push_back(t.path);
    Navigate(t, to, false);
}

static void GoUp(Tab& t) {
    if (t.path.empty()) return;
    std::wstring child = t.path;
    std::wstring parent = PathParent(child);
    Navigate(t, parent, true, ChildMarker(parent, child));
}

static int AddTab(const std::wstring& path, bool activate, int insertAt = -1) {
    Tab t;
    t.id = g_nextTabId++;
    t.path = NormalizeFolder(path);
    if (insertAt < 0 || insertAt > (int)g_tabs.size()) insertAt = (int)g_tabs.size();
    g_tabs.insert(g_tabs.begin() + insertAt, std::move(t));
    if (activate) g_active = insertAt;
    else if (insertAt <= g_active && g_tabs.size() > 1) ++g_active;
    Load(g_tabs[insertAt], false);
    if (activate) {
        WatchActive();
        g_focus = Focus::List;
    }
    Redraw();
    return insertAt;
}

static void ActivateTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    if (g_focus == Focus::Rename || g_focus == Focus::Path) g_focus = Focus::List;
    g_active = i;
    Tab& t = T();
    if (t.loaded && !t.loading && GetTickCount() - t.loadedAt > 1500) Load(t, true);
    WatchActive();
    if (g_focus == Focus::Search && t.filter.text.empty()) g_focus = Focus::List;
    Redraw();
}

static void CloseTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    g_closedTabs.push_back(g_tabs[i].path);
    if (g_closedTabs.size() > 25) g_closedTabs.erase(g_closedTabs.begin());
    if (g_tabs.size() == 1) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }
    g_tabs.erase(g_tabs.begin() + i);
    if (g_active > i || g_active >= (int)g_tabs.size()) g_active = std::max(0, g_active - 1);
    ActivateTab(g_active);
}

static void ReopenClosedTab() {
    if (g_closedTabs.empty()) return;
    std::wstring p = g_closedTabs.back();
    g_closedTabs.pop_back();
    AddTab(p, true, g_active + 1);
}

static std::wstring TabTitle(const Tab& t) {
    if (t.path.empty()) return L"This PC";
    if (t.path.size() == 3 && t.path[1] == L':') {
        auto it = g_driveNames.find(t.path);
        if (it != g_driveNames.end()) return it->second;
    }
    return PathLeaf(t.path);
}

// ============================================================ opening + commands
static void OpenFolderInNewTab(const std::wstring& path, bool activate) { AddTab(path, activate, g_active + 1); }

static void OpenView(Tab& t, int v, bool newTab) {
    if (v < 0 || v >= (int)t.view.size()) return;
    const Item& it = t.items[t.view[v]];
    std::wstring path = ItemPath(t, it);
    if (it.isDir()) {
        if (newTab) OpenFolderInNewTab(path, false);
        else Navigate(t, path);
        return;
    }
    std::wstring target;
    if (!OpenItem(g_hwnd, path, &target)) {
        if (newTab) OpenFolderInNewTab(target, false);
        else Navigate(t, target);
    }
}

static void OpenSelection(Tab& t, bool newTab) {
    std::vector<int> picks;
    for (int v = 0; v < (int)t.view.size(); ++v) if (t.sel[t.view[v]]) picks.push_back(v);
    if (picks.empty() && t.focus >= 0) picks.push_back(t.focus);
    if (picks.empty()) return;
    if (picks.size() > 15 && MessageBoxW(g_hwnd, (L"Open " + std::to_wstring(picks.size()) + L" items?").c_str(), L"Strata",
                                         MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    // Folders: the first navigates (or all open as tabs with Ctrl); files open with their programs
    bool navigated = false;
    std::vector<std::wstring> folders;
    for (int v : picks) {
        const Item& it = t.items[t.view[v]];
        if (it.isDir()) folders.push_back(ItemPath(t, it));
        else OpenView(t, v, newTab);
    }
    for (auto& f : folders) {
        if (!newTab && !navigated) { Navigate(t, f); navigated = true; }
        else OpenFolderInNewTab(f, false);
    }
}

static void CopyPathsToClipboard(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    std::wstring s;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i) s += L"\r\n";
        s += paths.size() > 1 ? (L"\"" + paths[i] + L"\"") : paths[i];
    }
    ClipboardSetText(g_hwnd, s);
    SetStatus(paths.size() == 1 ? L"Copied path" : L"Copied " + std::to_wstring(paths.size()) + L" paths");
}

static void DoCopyCut(bool cut) {
    Tab& t = T();
    if (DriveView(t)) return;
    auto paths = SelectedPaths(t);
    if (paths.empty()) return;
    if (ClipboardSetFiles(g_hwnd, paths, cut))
        SetStatus((cut ? L"Cut " : L"Copied ") + std::to_wstring(paths.size()) + (paths.size() == 1 ? L" item" : L" items"));
}

static void DoPaste(const std::wstring& destFolder) {
    if (destFolder.empty()) return;
    std::vector<std::wstring> files;
    bool cut = false;
    if (!ClipboardGetFiles(files, cut)) { SetStatus(L"Nothing to paste"); return; }
    if (cut) {
        // moving into the folder the items are already in does nothing
        bool same = true;
        for (auto& f : files) if (_wcsicmp(PathParent(f).c_str(), destFolder.c_str()) != 0) same = false;
        if (same) return;
    }
    FileOpAsync(g_hwnd, cut ? OpKind::Move : OpKind::Copy, files, destFolder);
    if (cut) { OpenClipboard(g_hwnd); EmptyClipboard(); CloseClipboard(); }
    Tab& t = T();
    std::fill(t.sel.begin(), t.sel.end(), 0);
    for (auto& f : files) t.restoreSel.insert(PathLeaf(f));
    SetStatus((cut ? L"Moving " : L"Copying ") + std::to_wstring(files.size()) + (files.size() == 1 ? L" item…" : L" items…"));
}

static void DoDelete(bool permanent) {
    Tab& t = T();
    if (DriveView(t)) return;
    auto paths = SelectedPaths(t);
    if (paths.empty()) return;
    FileOpAsync(g_hwnd, permanent ? OpKind::Delete : OpKind::Recycle, paths, L"");
}

static void SetFocusTo(Focus f);

static void BeginRename() {
    Tab& t = T();
    if (DriveView(t) || t.focus < 0 || t.focus >= (int)t.view.size()) return;
    int item = t.view[t.focus];
    if (!t.sel[item]) SelectOnly(t, t.focus);
    EnsureVisible(t, t.focus);
    g_renameItem = item;
    const Item& it = t.items[item];
    g_renameInput.SetText(it.name);
    size_t dot = it.name.find_last_of(L'.');
    if (!it.isDir() && dot != std::wstring::npos && dot > 0) g_renameInput.Select(0, dot);
    else g_renameInput.SelectAll();
    SetFocusTo(Focus::Rename);
}

static void CommitRename(bool commit) {
    if (g_focus != Focus::Rename) return;
    g_focus = Focus::List;
    Tab& t = T();
    if (commit && g_renameItem >= 0 && g_renameItem < (int)t.items.size()) {
        const Item& it = t.items[g_renameItem];
        std::wstring newName = g_renameInput.text;
        while (!newName.empty() && (newName.back() == L' ' || newName.back() == L'.')) newName.pop_back();
        while (!newName.empty() && newName.front() == L' ') newName.erase(newName.begin());
        if (!newName.empty() && newName != it.name) {
            std::wstring err;
            std::wstring oldPath = ItemPath(t, it);
            if (RenameItem(g_hwnd, oldPath, newName, &err)) {
                t.items[g_renameItem].name = newName;
                if (!t.items[g_renameItem].path.empty()) t.items[g_renameItem].path = PathJoin(PathParent(oldPath), newName);
                t.restoreSel = {newName};
                t.restoreFocus = newName;
                BuildView(t);
                for (int v = 0; v < (int)t.view.size(); ++v) if (t.items[t.view[v]].name == newName) { SelectOnly(t, v); EnsureVisible(t, v); }
            } else if (!err.empty()) {
                SetStatus(err);
            }
        }
    }
    g_renameItem = -1;
    Redraw();
}

static void NewFolder() {
    Tab& t = T();
    if (t.path.empty()) return;
    std::wstring name = UniqueName(t.path, L"New folder", L"");
    if (!CreateDirectoryW(PathJoin(t.path, name).c_str(), nullptr)) {
        SetStatus(L"Couldn't create a folder here: " + ErrorText(GetLastError()));
        return;
    }
    t.selectAfterLoad = name;
    t.renameAfterLoad = true;
    Load(t, false);
}

static void NewTextFile() {
    Tab& t = T();
    if (t.path.empty()) return;
    std::wstring name = UniqueName(t.path, L"New text document", L".txt");
    HANDLE h = CreateFileW(PathJoin(t.path, name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SetStatus(L"Couldn't create a file here: " + ErrorText(GetLastError())); return; }
    CloseHandle(h);
    t.selectAfterLoad = name;
    t.renameAfterLoad = true;
    Load(t, false);
}

static bool IsPinned(const std::wstring& path) {
    for (auto& p : g_config.pins) if (_wcsicmp(p.path.c_str(), path.c_str()) == 0) return true;
    return false;
}

static void TogglePin(const std::wstring& path) {
    if (path.empty()) return;
    for (size_t i = 0; i < g_config.pins.size(); ++i) {
        if (_wcsicmp(g_config.pins[i].path.c_str(), path.c_str()) == 0) {
            g_config.pins.erase(g_config.pins.begin() + i);
            ConfigSave();
            SetStatus(L"Unpinned " + PathLeaf(path));
            return;
        }
    }
    g_config.pins.push_back({NormalizeFolder(path), L"", 0});
    ConfigSave();
    SetStatus(L"Pinned " + PathLeaf(path) + L" to the sidebar");
}

static void NavigateTyped(const std::wstring& input) {
    std::wstring s = input;
    while (!s.empty() && (s.front() == L' ' || s.front() == L'"')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'"')) s.pop_back();
    Tab& t = T();
    if (s.empty() || _wcsicmp(s.c_str(), L"This PC") == 0) { Navigate(t, L""); return; }
    wchar_t exp[4096];
    ExpandEnvironmentStringsW(s.c_str(), exp, (DWORD)std::size(exp));
    std::wstring p = exp;
    if (p == L"~") {
        PWSTR prof = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &prof))) p = prof;
        CoTaskMemFree(prof);
    }
    // relative to the current folder
    if (!t.path.empty() && p.find(L':') == std::wstring::npos && p.rfind(L"\\\\", 0) != 0) {
        std::wstring rel = PathJoin(t.path, p);
        wchar_t full[4096];
        if (GetFullPathNameW(rel.c_str(), (DWORD)std::size(full), full, nullptr) && GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) p = full;
    }
    DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { Navigate(t, p); return; }
    if (a != INVALID_FILE_ATTRIBUTES) { Navigate(t, PathParent(p), true, PathLeaf(p)); return; }
    // Like Explorer, a bare command name runs it (e.g. "cmd", "notepad")
    if (p.find(L'\\') == std::wstring::npos) {
        if ((INT_PTR)ShellExecuteW(g_hwnd, nullptr, p.c_str(), nullptr, t.path.empty() ? nullptr : t.path.c_str(), SW_SHOWNORMAL) > 32) return;
    }
    SetStatus(L"Can't find \"" + s + L"\"");
}

// ============================================================ painting helpers
static D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
static bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static void AddZone(ZoneKind k, int idx, const D2D1_RECT_F& r) { g_zones.push_back({k, idx, r}); }
static bool Hot(ZoneKind k, int idx = -1) { return g_hotKind == k && (idx < 0 || g_hotIdx == idx); }

static D2D1_COLOR_F ChromeColor() { return Alpha(g_theme.bottomBg, g_theme.backdrop ? g_theme.opacity * 0.85f : 1.0f); }
static D2D1_COLOR_F MainColor() { return Alpha(g_theme.bg, g_theme.backdrop ? g_theme.opacity : 1.0f); }
static D2D1_COLOR_F SidebarColor() { return Alpha(Mix(g_theme.bg, g_theme.bottomBg, 0.55f), g_theme.backdrop ? g_theme.opacity * 0.92f : 1.0f); }

static void DrawIconButton(ZoneKind k, const D2D1_RECT_F& r, wchar_t glyph, bool enabled, IDWriteTextFormat* f = nullptr) {
    AddZone(k, -1, r);
    if (enabled && Hot(k)) g_cv.FillRound(r, 6, g_theme.hover);
    g_cv.Glyph(glyph, f ? f : g_fIcon, r, enabled ? g_theme.text : Alpha(g_theme.subtext, 0.45f));
}

static void DrawLogo(float x, float cy, float s) {
    // three stacked layers: Strata's mark
    float w = s, h = s * 0.2f, gap = s * 0.1f;
    float top = cy - (h * 3 + gap * 2) / 2;
    g_cv.FillRound(R(x + w * 0.18f, top, x + w, top + h), h / 2, Alpha(g_theme.accent, 0.55f));
    g_cv.FillRound(R(x + w * 0.08f, top + h + gap, x + w * 0.92f, top + 2 * h + gap), h / 2, Alpha(g_theme.accent, 0.8f));
    g_cv.FillRound(R(x, top + 2 * (h + gap), x + w * 0.82f, top + 3 * h + 2 * gap), h / 2, g_theme.accent);
}

static void DrawItemIcon(int index, float x, float cy, float size, float opacity = 1.0f) {
    ID2D1Bitmap* bmp = IconBitmap(g_rt, index);
    D2D1_RECT_F r = R(x, cy - size / 2, x + size, cy + size / 2);
    if (bmp) g_rt->DrawBitmap(bmp, r, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    else g_cv.Glyph(0xE8B7, g_fIcon, r, Alpha(g_theme.accent, opacity));
}

// ============================================================ title bar + tabs
static float TabsLeft() { return 12 + 18 + 10 + MeasureWidth(L"Strata", g_fTitle) + 14; }

static void TabGeometry(float& left, float& tabW) {
    left = TabsLeft();
    float right = g_W - 3 * kCapW - 44;
    float avail = std::max(60.0f, right - left);
    tabW = std::clamp(avail / std::max<size_t>(1, g_tabs.size()), 56.0f, 220.0f);
}

static void PaintTitleBar() {
    bool activeWnd = GetForegroundWindow() == g_hwnd;
    D2D1_COLOR_F textC = activeWnd ? g_theme.text : g_theme.subtext;
    g_cv.Fill(R(0, 0, g_W, kTitleH), ChromeColor());
    DrawLogo(14, kTitleH / 2, 18);
    g_cv.Text(L"Strata", g_fTitle, R(14 + 18 + 10, 0, 200, kTitleH), textC);

    float left, tabW;
    TabGeometry(left, tabW);
    float top = 6, bottom = kTitleH;
    int folderIcon = FolderIconIndex();
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        float x = left + i * tabW;
        if (g_drag == Drag::Tab && g_dragMoved && i == g_dragIdx) continue;   // drawn last, under the cursor
        D2D1_RECT_F r = R(x + 2, top, x + tabW - 2, bottom);
        bool act = i == g_active;
        if (act) {
            // active tab joins the toolbar below
            g_cv.FillRound(R(r.left, r.top, r.right, r.bottom + 8), 8, MainColor());
        } else if (Hot(ZTab, i) || Hot(ZTabClose, i)) {
            g_cv.FillRound(R(r.left, r.top + 2, r.right, r.bottom - 4), 7, g_theme.hover);
        } else if (i + 1 != g_active && i + 1 < (int)g_tabs.size()) {
            g_cv.Fill(R(x + tabW - 0.5f, top + 10, x + tabW + 0.5f, bottom - 10), g_theme.divider);
        }
        AddZone(ZTab, i, r);
        const Tab& t = g_tabs[i];
        float cy = (r.top + r.bottom) / 2 - (act ? 0 : 1);
        bool showClose = act || Hot(ZTab, i) || Hot(ZTabClose, i);
        float textRight = r.right - (showClose ? 30 : 10);
        if (t.path.empty()) g_cv.Glyph(0xE7F8, g_fIcon, R(r.left + 8, cy - 8, r.left + 26, cy + 8), g_theme.accent);
        else DrawItemIcon(folderIcon, r.left + 10, cy, 16);
        g_cv.Text(TabTitle(t), act ? g_fUiSemi : g_fUi, R(r.left + 32, r.top, textRight, r.bottom - (act ? 0 : 2)), act ? g_theme.text : textC);
        if (showClose && tabW > 80) {
            D2D1_RECT_F cr = R(r.right - 28, cy - 11, r.right - 6, cy + 11);
            AddZone(ZTabClose, i, cr);
            if (Hot(ZTabClose, i)) g_cv.FillRound(cr, 5, g_theme.hover);
            g_cv.Glyph(0xE711, g_fIconSmall, cr, g_theme.subtext);
        }
    }
    if (g_drag == Drag::Tab && g_dragMoved && g_dragIdx >= 0 && g_dragIdx < (int)g_tabs.size()) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(g_hwnd, &pt);
        float x = std::clamp(Px2Dip(pt.x) - g_dragOrig, left, left + (g_tabs.size() - 1) * tabW);
        D2D1_RECT_F r = R(x + 2, top, x + tabW - 2, bottom);
        g_cv.FillRound(R(r.left, r.top, r.right, r.bottom + 8), 8, Mix(g_theme.bg, g_theme.surfaceStrong, 0.5f));
        const Tab& t = g_tabs[g_dragIdx];
        float cy = (r.top + r.bottom) / 2;
        if (t.path.empty()) g_cv.Glyph(0xE7F8, g_fIcon, R(r.left + 8, cy - 8, r.left + 26, cy + 8), g_theme.accent);
        else DrawItemIcon(folderIcon, r.left + 10, cy, 16);
        g_cv.Text(TabTitle(t), g_fUiSemi, R(r.left + 32, r.top, r.right - 10, r.bottom), g_theme.text);
    }
    // new tab
    float nx = left + g_tabs.size() * tabW + 4;
    nx = std::min(nx, g_W - 3 * kCapW - 40);
    DrawIconButton(ZNewTab, R(nx, 8, nx + 30, kTitleH - 4), 0xE710, true);

    // caption buttons
    bool maxed = IsZoomed(g_hwnd) != FALSE;
    struct Cap { ZoneKind k; wchar_t g; } caps[] = {{ZCapMin, 0xE921}, {ZCapMax, maxed ? (wchar_t)0xE923 : (wchar_t)0xE922}, {ZCapClose, 0xE8BB}};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F r = R(g_W - (3 - i) * kCapW, 0, g_W - (2 - i) * kCapW, kTitleH - 4);
        AddZone(caps[i].k, -1, r);
        bool hot = Hot(caps[i].k);
        if (hot) g_cv.Fill(r, caps[i].k == ZCapClose ? Rgb(0xC42B1C) : g_theme.hover);
        g_cv.Glyph(caps[i].g, g_fIconSmall, r, hot && caps[i].k == ZCapClose ? Rgb(0xFFFFFF) : textC);
    }
}

// ============================================================ toolbar + breadcrumb
struct Crumb { std::wstring label, path; D2D1_RECT_F rc, arrow; };
static std::vector<Crumb> g_crumbs;
static std::vector<std::wstring> g_hiddenCrumbs;   // ancestors elided from the bar

static void BuildCrumbs(float boxLeft, float boxRight) {
    g_crumbs.clear();
    g_hiddenCrumbs.clear();
    const Tab& t = T();
    std::vector<std::wstring> full;
    std::vector<std::wstring> parts = SplitPath(t.path, &full);
    std::vector<Crumb> all;
    all.push_back({L"This PC", L"", {}, {}});
    for (size_t i = 0; i < parts.size(); ++i) {
        std::wstring label = parts[i];
        if (i == 0) {
            auto it = g_driveNames.find(full[0]);
            if (it != g_driveNames.end()) label = it->second;
        }
        all.push_back({label, full[i], {}, {}});
    }
    const float pad = 8, arrowW = 18;
    std::vector<float> widths;
    for (size_t i = 0; i < all.size(); ++i)   // the current folder is drawn semibold
        widths.push_back(std::min(240.0f, MeasureWidth(all[i].label, i + 1 == all.size() ? g_fUiSemi : g_fUi) + pad * 2 + 2));
    widths[0] = 30;   // This PC is shown as an icon
    float avail = boxRight - boxLeft - 34;   // leave room for typing area
    size_t first = 0;
    auto total = [&](size_t from) {
        float w = from > 0 ? 28 : 0;
        for (size_t i = from; i < all.size(); ++i) w += widths[i] + arrowW;
        return w;
    };
    while (first + 1 < all.size() && total(first) > avail) ++first;
    for (size_t i = 1; i < first; ++i) g_hiddenCrumbs.push_back(all[i].path);
    if (first > 0) g_hiddenCrumbs.insert(g_hiddenCrumbs.begin(), L"\x01");   // marker for This PC
    float x = boxLeft + 2;
    if (first > 0) {
        Crumb ov{L"…", L"", R(x, 0, x + 28, 0), {}};
        g_crumbs.push_back(ov);   // overflow marker, index 0 when present
        x += 28;
    }
    for (size_t i = first; i < all.size(); ++i) {
        Crumb c = all[i];
        c.rc = R(x, 0, x + widths[i], 0);
        x += widths[i];
        c.arrow = R(x, 0, x + arrowW, 0);
        x += arrowW;
        g_crumbs.push_back(c);
    }
}

static void PaintToolbar() {
    float top = kTitleH, bottom = kTitleH + kToolH;
    g_cv.Fill(R(0, top, g_W, bottom), MainColor());
    Tab& t = T();
    float cy = (top + bottom) / 2, bh = 32;
    float x = 10;
    DrawIconButton(ZBack, R(x, cy - bh / 2, x + 34, cy + bh / 2), 0xE72B, !t.back.empty()); x += 36;
    DrawIconButton(ZFwd, R(x, cy - bh / 2, x + 34, cy + bh / 2), 0xE72A, !t.fwd.empty()); x += 36;
    DrawIconButton(ZUp, R(x, cy - bh / 2, x + 34, cy + bh / 2), 0xE74A, !t.path.empty()); x += 36;
    DrawIconButton(ZRefresh, R(x, cy - bh / 2, x + 34, cy + bh / 2), 0xE72C, true); x += 42;

    float menuW = 34;
    float searchW = std::clamp(g_W * 0.24f, 180.0f, 320.0f);
    float right = g_W - 10;
    D2D1_RECT_F menu = R(right - menuW, cy - bh / 2, right, cy + bh / 2);
    DrawIconButton(ZMenu, menu, 0xE712, true);
    right -= menuW + 8;
    g_rcSearchBox = R(right - searchW, cy - 16, right, cy + 16);
    right -= searchW + 8;
    g_rcPathBox = R(x, cy - 16, right, cy + 16);

    // path box
    float rad = std::min(8.0f, g_theme.radius + 2);
    bool pathFocus = g_focus == Focus::Path;
    g_cv.FillRound(g_rcPathBox, rad, pathFocus ? g_theme.surfaceStrong : g_theme.surface);
    if (pathFocus) g_cv.StrokeRound(g_rcPathBox, rad, Alpha(g_theme.accent, 0.9f), 1.2f);
    else if (Hot(ZPathBox) || Hot(ZCrumb) || Hot(ZCrumbArrow) || Hot(ZCrumbOverflow)) g_cv.StrokeRound(g_rcPathBox, rad, g_theme.divider, 1);
    AddZone(ZPathBox, -1, g_rcPathBox);
    if (pathFocus) {
        D2D1_RECT_F tb = R(g_rcPathBox.left + 12, g_rcPathBox.top, g_rcPathBox.right - 10, g_rcPathBox.bottom);
        g_pathInput.Draw(g_cv, g_fInput, tb, g_theme.text, Alpha(g_theme.accent, 0.35f), g_theme.text, true);
    } else {
        BuildCrumbs(g_rcPathBox.left, g_rcPathBox.right);
        D2D1_RECT_F clip = R(g_rcPathBox.left + 1, g_rcPathBox.top, g_rcPathBox.right - 1, g_rcPathBox.bottom);
        g_rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        float ct = g_rcPathBox.top + 3, cb = g_rcPathBox.bottom - 3;
        for (int i = 0; i < (int)g_crumbs.size(); ++i) {
            Crumb& c = g_crumbs[i];
            c.rc.top = c.arrow.top = ct;
            c.rc.bottom = c.arrow.bottom = cb;
            bool overflow = c.label == L"…" && c.path.empty() && !g_hiddenCrumbs.empty() && i == 0;
            if (overflow) {
                AddZone(ZCrumbOverflow, i, c.rc);
                if (Hot(ZCrumbOverflow, i)) g_cv.FillRound(c.rc, 5, g_theme.hover);
                g_cv.Glyph(0xE76B, g_fIconSmall, c.rc, g_theme.subtext);
                continue;
            }
            AddZone(ZCrumb, i, c.rc);
            if (Hot(ZCrumb, i)) g_cv.FillRound(c.rc, 5, g_theme.hover);
            bool last = i + 1 == (int)g_crumbs.size();
            if (c.label == L"This PC" && c.path.empty()) g_cv.Glyph(0xE7F8, g_fIcon, c.rc, last ? g_theme.text : g_theme.subtext);
            else g_cv.Text(c.label, last ? g_fUiSemi : g_fUi, R(c.rc.left + 8, c.rc.top, c.rc.right - 6, c.rc.bottom), last ? g_theme.text : g_theme.subtext);
            AddZone(ZCrumbArrow, i, c.arrow);
            if (Hot(ZCrumbArrow, i)) g_cv.FillRound(c.arrow, 5, g_theme.hover);
            g_cv.Glyph(0xE974, g_fIconSmall, c.arrow, Alpha(g_theme.subtext, 0.85f));
        }
        g_rt->PopAxisAlignedClip();
    }

    // search box
    bool sFocus = g_focus == Focus::Search;
    g_cv.FillRound(g_rcSearchBox, rad, sFocus ? g_theme.surfaceStrong : g_theme.surface);
    if (sFocus) g_cv.StrokeRound(g_rcSearchBox, rad, Alpha(g_theme.accent, 0.9f), 1.2f);
    else if (Hot(ZSearch)) g_cv.StrokeRound(g_rcSearchBox, rad, g_theme.divider, 1);
    AddZone(ZSearch, -1, g_rcSearchBox);
    g_cv.Glyph(0xE721, g_fIconSmall, R(g_rcSearchBox.left + 8, g_rcSearchBox.top, g_rcSearchBox.left + 28, g_rcSearchBox.bottom),
               sFocus ? g_theme.accent : g_theme.subtext);
    D2D1_RECT_F stb = R(g_rcSearchBox.left + 32, g_rcSearchBox.top, g_rcSearchBox.right - 30, g_rcSearchBox.bottom);
    if (t.filter.text.empty() && !sFocus) {
        g_cv.Text(t.path.empty() ? std::wstring(L"Search all drives") : L"Search " + TabTitle(t), g_fUi, stb, Alpha(g_theme.subtext, 0.8f));
        g_cv.Text(L"Ctrl+K", g_fSmallRight, R(stb.left, stb.top, g_rcSearchBox.right - 10, stb.bottom), Alpha(g_theme.subtext, 0.55f));
    } else {
        t.filter.Draw(g_cv, g_fInput, stb, g_theme.text, Alpha(g_theme.accent, 0.35f), g_theme.text, sFocus);
        if (!t.filter.text.empty()) {
            D2D1_RECT_F cr = R(g_rcSearchBox.right - 28, g_rcSearchBox.top + 5, g_rcSearchBox.right - 6, g_rcSearchBox.bottom - 5);
            AddZone(ZSearchClear, -1, cr);
            if (Hot(ZSearchClear)) g_cv.FillRound(cr, 5, g_theme.hover);
            g_cv.Glyph(0xE711, g_fIconSmall, cr, g_theme.subtext);
        }
    }
}

// ============================================================ sidebar
static void PaintSidebar() {
    float top = kTitleH + kToolH, bottom = g_H - kStatusH;
    float w = g_config.sidebarWidth;
    g_rcSidebar = R(0, top, w, bottom);
    g_cv.Fill(g_rcSidebar, SidebarColor());
    AddZone(ZSideSplitter, -1, R(w - kSplitW / 2, top, w + kSplitW / 2, bottom));
    if (g_drag == Drag::Splitter || Hot(ZSideSplitter)) g_cv.Fill(R(w - 1, top, w + 1, bottom), Alpha(g_theme.accent, 0.6f));
    else g_cv.Fill(R(w - 0.5f, top, w + 0.5f, bottom), g_theme.divider);

    g_rt->PushAxisAlignedClip(R(0, top, w - 1, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = top + 8 - g_sideScroll;
    const Tab& t = T();
    auto rowBg = [&](const D2D1_RECT_F& r, bool current, bool hot) {
        if (current) {
            g_cv.FillRound(r, 6, Alpha(g_theme.accent, 0.16f));
            g_cv.FillRound(R(r.left, r.top + 8, r.left + 3, r.bottom - 8), 1.5f, g_theme.accent);
        } else if (hot) {
            g_cv.FillRound(r, 6, g_theme.hover);
        }
    };
    // This PC
    {
        D2D1_RECT_F r = R(8, y, w - 8, y + 32);
        AddZone(ZSideThisPC, -1, r);
        rowBg(r, t.path.empty(), Hot(ZSideThisPC));
        g_cv.Glyph(0xE7F8, g_fIcon, R(r.left + 8, r.top, r.left + 30, r.bottom), g_theme.accent);
        g_cv.Text(L"This PC", g_fUi, R(r.left + 38, r.top, r.right - 8, r.bottom), g_theme.text);
        y += 36;
    }
    // Pinned
    g_cv.Text(L"PINNED", g_fSection, R(20, y, w - 8, y + 26), g_theme.subtext);
    y += 26;
    int folderIcon = FolderIconIndex();
    if (g_config.pins.empty()) {
        g_cv.Text(L"Right-click a folder → Pin", g_fSmall, R(20, y, w - 8, y + 26), Alpha(g_theme.subtext, 0.7f));
        y += 28;
    }
    for (int i = 0; i < (int)g_config.pins.size(); ++i) {
        const Pin& p = g_config.pins[i];
        D2D1_RECT_F r = R(8, y, w - 8, y + 32);
        AddZone(ZSidePin, i, r);
        bool cur = !t.path.empty() && _wcsicmp(t.path.c_str(), p.path.c_str()) == 0;
        rowBg(r, cur, Hot(ZSidePin, i));
        DrawItemIcon(folderIcon, r.left + 10, (r.top + r.bottom) / 2, 16);
        float textRight = r.right - (p.color ? 22 : 8);
        std::wstring name = p.name.empty() ? PathLeaf(p.path) : p.name;
        g_cv.Text(name, g_fUi, R(r.left + 38, r.top, textRight, r.bottom), g_theme.text);
        if (p.color) g_cv.Circle(r.right - 12, (r.top + r.bottom) / 2, 4, Rgb(p.color));
        y += 34;
    }
    // Drives
    y += 6;
    g_cv.Text(L"DRIVES", g_fSection, R(20, y, w - 8, y + 26), g_theme.subtext);
    y += 26;
    for (int i = 0; i < (int)g_drives.size(); ++i) {
        const DriveInfo& d = g_drives[i];
        D2D1_RECT_F r = R(8, y, w - 8, y + 50);
        AddZone(ZSideDrive, i, r);
        bool cur = !t.path.empty() && t.path.size() >= 3 && _wcsnicmp(t.path.c_str(), d.root.c_str(), 3) == 0;
        rowBg(r, cur && t.path.size() == 3, Hot(ZSideDrive, i));
        DrawItemIcon(d.icon, r.left + 10, r.top + 17, 16);
        g_cv.Text(DriveDisplayName(d), g_fUi, R(r.left + 38, r.top + 4, r.right - 8, r.top + 28), g_theme.text);
        if (d.ready && d.total) {
            float used = 1.0f - (float)((double)d.freeBytes / (double)d.total);
            D2D1_RECT_F bar = R(r.left + 38, r.top + 30, r.right - 10, r.top + 34);
            g_cv.FillRound(bar, 2, g_theme.surfaceStrong);
            D2D1_COLOR_F c = used > 0.9f ? Rgb(0xE5484D) : used > 0.8f ? Rgb(0xF5A524) : g_theme.accent;
            g_cv.FillRound(R(bar.left, bar.top, bar.left + (bar.right - bar.left) * std::clamp(used, 0.02f, 1.0f), bar.bottom), 2, c);
            g_cv.Text(FormatSize(d.freeBytes) + L" free of " + FormatSize(d.total), g_fSmall, R(r.left + 38, r.top + 34, r.right - 8, r.bottom - 1),
                      g_theme.subtext);
        } else {
            g_cv.Text(L"Not ready", g_fSmall, R(r.left + 38, r.top + 30, r.right - 8, r.bottom - 4), g_theme.subtext);
        }
        y += 52;
    }
    g_sideContentH = y + g_sideScroll - top + 8;
    g_rt->PopAxisAlignedClip();
}

// ============================================================ file list
struct ColGeom { int col; float left, right; };
static std::vector<ColGeom> g_cols;
static const wchar_t* kColLabel[ColCount] = {L"Name", L"Size", L"Modified", L"Type"};

static void ComputeColumns(float left) {
    g_cols.clear();
    float x = left;
    bool thisPC = DriveView(T());
    for (int c = 0; c < ColCount; ++c) {
        if (!g_config.columnVisible[c]) continue;
        float w = g_config.columnWidth[c];
        if (thisPC && c == ColModified) w = std::max(w, 200.0f);
        if (T().searchActive && c == ColType) w = std::max(w, 360.0f);   // Location
        g_cols.push_back({c, x, x + w});
        x += w;
    }
}

static float g_rowRight = 1e9f;   // right edge of drawn rows; clicks past it hit the background

static int RowAt(float y, float x = -1) {
    const Tab& t = T();
    if (y < g_rcList.top || y >= g_rcList.bottom) return -1;
    if (x >= 0 && x > g_rowRight) return -1;
    int v = (int)std::floor((y - g_rcList.top + t.scroll) / RowH());
    return v >= 0 && v < (int)t.view.size() ? v : -1;
}

static D2D1_RECT_F ScrollTrack() { return R(g_rcList.right - 12, g_rcList.top + 2, g_rcList.right - 2, g_rcList.bottom - 2); }

static bool ScrollThumb(D2D1_RECT_F& thumb) {
    const Tab& t = T();
    float content = t.view.size() * RowH() + 8, h = ListViewH();
    if (content <= h + 1) return false;
    D2D1_RECT_F tr = ScrollTrack();
    float th = std::max(32.0f, (tr.bottom - tr.top) * h / content);
    float maxS = MaxScroll(t);
    float pos = maxS > 0 ? t.scroll / maxS : 0;
    float y = tr.top + (tr.bottom - tr.top - th) * pos;
    thumb = R(tr.left, y, tr.right, y + th);
    return true;
}

static void PaintList() {
    Tab& t = T();
    float left = g_config.sidebarWidth, top = kTitleH + kToolH, bottom = g_H - kStatusH;
    g_cv.Fill(R(left, top, g_W, bottom), MainColor());
    g_rcHeader = R(left, top, g_W, top + kHeaderH);
    g_rcList = R(left, top + kHeaderH, g_W, bottom);
    ClampScroll(t);
    ComputeColumns(left + 12);
    bool thisPC = DriveView(t);

    // header
    g_rt->PushAxisAlignedClip(g_rcHeader, D2D1_ANTIALIAS_MODE_ALIASED);
    for (auto& c : g_cols) {
        D2D1_RECT_F r = R(c.left, top, c.right, top + kHeaderH);
        AddZone(ZHeader, c.col, r);
        if (Hot(ZHeader, c.col)) g_cv.FillRound(R(r.left, r.top + 4, r.right, r.bottom - 4), 5, g_theme.hover);
        bool sorted = g_config.sortColumn == c.col;
        bool right = c.col == ColSize;
        const wchar_t* label = kColLabel[c.col];
        if (thisPC && c.col == ColModified) label = L"Space";
        if (t.searchActive && c.col == ColType) label = L"Location";
        float tw = MeasureWidth(label, g_fHeader);
        D2D1_RECT_F tr = right ? R(r.right - 12 - tw, r.top, r.right - 12, r.bottom) : R(r.left + 10, r.top, r.right - 18, r.bottom);
        g_cv.Text(label, g_fHeader, tr, sorted ? g_theme.text : g_theme.subtext);
        if (sorted) {
            float ax = right ? tr.left - 16 : std::min(tr.left + tw + 4, r.right - 16);
            g_cv.Glyph(g_config.sortDescending ? 0xE96E : 0xE96D, g_fIconSmall, R(ax, r.top, ax + 14, r.bottom), g_theme.accent);
        }
        D2D1_RECT_F div = R(c.right - 4, top + 6, c.right + 4, top + kHeaderH - 6);
        AddZone(ZHeaderDiv, c.col, div);
        g_cv.Fill(R(c.right - 0.5f, top + 9, c.right + 0.5f, top + kHeaderH - 9),
                  Hot(ZHeaderDiv, c.col) || (g_drag == Drag::Column && g_dragIdx == c.col) ? g_theme.accent : g_theme.divider);
    }
    g_rt->PopAxisAlignedClip();
    g_cv.Fill(R(left, g_rcHeader.bottom - 0.5f, g_W, g_rcHeader.bottom + 0.5f), g_theme.divider);
    AddZone(ZList, -1, g_rcList);

    // empty / loading / error states
    float cx = (g_rcList.left + g_rcList.right) / 2, cy = g_rcList.top + std::min(160.0f, ListViewH() * 0.35f);
    if (t.view.empty()) {
        std::wstring title, sub;
        wchar_t glyph = 0xE8B7;
        if (t.searchActive && t.searchRunning) {
            glyph = 0xE721;
            title = L"Searching…";
            sub = t.path.empty() ? L"Looking through every drive" : L"Looking through " + TabTitle(t) + L" and its subfolders";
            if (t.searchScanned) sub += L" (" + WithCommas(t.searchScanned) + L" folders so far)";
        } else if (t.searchActive) {
            glyph = 0xE721;
            title = L"No results";
            sub = L"Nothing found for \"" + t.searchQuery + L"\"";
        } else if (t.loading && !t.loaded) {
            title = L"Loading…";
        } else if (t.error) {
            glyph = 0xE783;
            title = L"Can't open this folder";
            sub = ErrorText(t.error);
        } else if (!t.filter.text.empty()) {
            glyph = 0xE721;
            title = L"No matches";
            sub = L"Nothing in this folder matches \"" + t.filter.text + L"\" — press Enter to search subfolders";
        } else if (t.loaded) {
            title = L"This folder is empty";
            sub = L"Paste files here with Ctrl+V, or right-click for New";
        }
        if (!title.empty()) {
            if (!(t.loading && !t.loaded)) g_cv.Glyph(glyph, g_fIconBig, R(cx - 40, cy - 30, cx + 40, cy + 30), Alpha(g_theme.subtext, 0.55f));
            g_cv.Text(title, g_fEmpty, R(g_rcList.left + 20, cy + 34, g_rcList.right - 20, cy + 60), g_theme.text);
            if (!sub.empty()) g_cv.Text(sub, g_fCenter, R(g_rcList.left + 20, cy + 60, g_rcList.right - 20, cy + 82), g_theme.subtext);
        }
    }

    // rows (virtualized: only the visible range is drawn)
    float rh = RowH();
    int first = std::max(0, (int)std::floor(t.scroll / rh));
    int last = std::min((int)t.view.size() - 1, (int)std::ceil((t.scroll + ListViewH()) / rh));
    g_rt->PushAxisAlignedClip(g_rcList, D2D1_ANTIALIAS_MODE_ALIASED);
    bool listFocus = g_focus == Focus::List || g_focus == Focus::Rename;
    float rowRight = g_cols.empty() ? g_rcList.right - 14 : g_cols.back().right + 6;
    rowRight = std::min(g_rcList.right - 14, std::max(rowRight, g_rcList.left + 300));
    g_rowRight = rowRight;
    g_rcRenameBox = R(0, 0, 0, 0);
    for (int v = first; v <= last; ++v) {
        int idx = t.view[v];
        const Item& it = t.items[idx];
        float y = g_rcList.top + v * rh - t.scroll;
        D2D1_RECT_F row = R(g_rcList.left + 6, y + 1, rowRight, y + rh - 1);
        bool sel = t.sel[idx] != 0;
        if (sel) g_cv.FillRound(row, 6, Alpha(g_theme.accent, listFocus ? 0.24f : 0.13f));
        else if (g_hotRow == v && g_drag == Drag::None) g_cv.FillRound(row, 6, g_theme.hover);
        if (v == t.focus && listFocus && !sel) g_cv.StrokeRound(row, 6, Alpha(g_theme.accent, 0.45f), 1);
        float ry = y + rh / 2;
        float fade = (it.attrs & FILE_ATTRIBUTE_HIDDEN) ? 0.55f : 1.0f;
        D2D1_COLOR_F tc = Alpha(g_theme.text, fade), sc = Alpha(g_theme.subtext, fade);
        for (auto& c : g_cols) {
            D2D1_RECT_F cell = R(c.left + 10, y, c.right - 10, y + rh);
            switch (c.col) {
                case ColName: {
                    DrawItemIcon(it.icon, c.left + 8, ry, 16, fade);
                    D2D1_RECT_F nr = R(c.left + 34, y, c.right - 8, y + rh);
                    if (g_focus == Focus::Rename && idx == g_renameItem) {
                        g_rcRenameBox = R(nr.left - 4, y + 3, std::max(nr.right, nr.left + 220), y + rh - 3);
                        g_cv.FillRound(g_rcRenameBox, 4, g_theme.surfaceStrong);
                        g_cv.StrokeRound(g_rcRenameBox, 4, g_theme.accent, 1.2f);
                        g_renameInput.Draw(g_cv, g_fInput, R(g_rcRenameBox.left + 4, g_rcRenameBox.top, g_rcRenameBox.right - 4, g_rcRenameBox.bottom),
                                           g_theme.text, Alpha(g_theme.accent, 0.4f), g_theme.text, true);
                    } else {
                        g_cv.Text(DisplayName(it), g_fUi, nr, tc);
                    }
                    break;
                }
                case ColSize:
                    if (thisPC) g_cv.Text(it.total ? FormatSize(it.total) : L"", g_fRight, cell, sc);
                    else if (!it.isDir()) g_cv.Text(FormatSize(it.size), g_fRight, cell, sc);
                    break;
                case ColModified:
                    if (thisPC) {
                        if (it.total) {
                            float used = 1.0f - (float)((double)it.freeBytes / (double)it.total);
                            D2D1_RECT_F bar = R(cell.left, ry - 2, cell.left + 90, ry + 2);
                            g_cv.FillRound(bar, 2, g_theme.surfaceStrong);
                            D2D1_COLOR_F bc = used > 0.9f ? Rgb(0xE5484D) : used > 0.8f ? Rgb(0xF5A524) : g_theme.accent;
                            g_cv.FillRound(R(bar.left, bar.top, bar.left + 90 * std::clamp(used, 0.02f, 1.0f), bar.bottom), 2, bc);
                            g_cv.Text(FormatSize(it.freeBytes) + L" free", g_fSmall, R(cell.left + 100, y, cell.right, y + rh), sc);
                        }
                    } else {
                        g_cv.Text(FormatDate(it.modified), g_fUi, cell, sc);
                    }
                    break;
                case ColType:
                    if (t.searchActive) g_cv.Text(PathParent(it.path), g_fUi, cell, sc);
                    else g_cv.Text(t.types[it.type], g_fUi, cell, sc);
                    break;
            }
        }
    }
    // marquee
    if (g_drag == Drag::Marquee && g_dragMoved) {
        D2D1_RECT_F m = R(std::min(g_marqueeA.x, g_marqueeB.x), std::min(g_marqueeA.y, g_marqueeB.y) - t.scroll + g_rcList.top,
                          std::max(g_marqueeA.x, g_marqueeB.x), std::max(g_marqueeA.y, g_marqueeB.y) - t.scroll + g_rcList.top);
        g_cv.Fill(m, Alpha(g_theme.accent, 0.15f));
        g_cv.StrokeRound(m, 0, Alpha(g_theme.accent, 0.7f), 1);
    }
    // overlay scrollbar
    D2D1_RECT_F thumb;
    if (ScrollThumb(thumb)) {
        AddZone(ZScrollbar, -1, ScrollTrack());
        bool hot = Hot(ZScrollbar) || g_drag == Drag::Scrollbar;
        if (!hot) thumb.left += 4;
        g_cv.FillRound(thumb, (thumb.right - thumb.left) / 2, Alpha(g_theme.text, hot ? 0.38f : 0.22f));
    }
    g_rt->PopAxisAlignedClip();
}

// ============================================================ status bar
static void PaintStatus() {
    float top = g_H - kStatusH;
    D2D1_RECT_F r = R(0, top, g_W, g_H);
    g_cv.Fill(r, ChromeColor());
    g_cv.Fill(R(0, top, g_W, top + 0.5f), g_theme.divider);
    Tab& t = T();
    std::wstring left;
    if (!g_statusMsg.empty()) {
        left = g_statusMsg;
    } else {
        size_t total = t.view.size();
        left = WithCommas(total) + (t.searchActive ? (total == 1 ? L" result" : L" results") : (total == 1 ? L" item" : L" items"));
        if (t.searchActive) {
            if (t.searchRunning) left += L"   ·   searching" + (t.searchScanned ? L" (" + WithCommas(t.searchScanned) + L" folders)" : std::wstring()) + L"…";
            else if (t.searchCapped && t.searchEverything && t.searchTotal) left += L"   ·   first " + WithCommas(total) + L" of " + WithCommas(t.searchTotal);
            else if (t.searchCapped) left += L"   ·   stopped at " + WithCommas(kSearchMaxResults) + L" — be more specific";
            if (t.searchEverything) left += L"   ·   via Everything";
            else if (!t.searchRunning && t.path.empty()) left += L"   ·   tip: install Everything for instant whole-PC search";
        } else if (!t.filter.text.empty() && !t.path.empty()) {
            left += L" match   ·   Enter searches subfolders";
        }
        int n = 0;
        uint64_t bytes = 0;
        bool anyDir = false;
        for (int v : t.view) if (t.sel[v]) { ++n; bytes += t.items[v].isDir() ? 0 : t.items[v].size; anyDir |= t.items[v].isDir(); }
        if (n) {
            left += L"   ·   " + WithCommas(n) + L" selected";
            if (bytes || !anyDir) left += L"  (" + FormatSize(bytes) + L")";
        }
        size_t hiddenCount = 0;
        if (!g_config.showHidden) for (auto& it : t.items) if (ItemHidden(it)) ++hiddenCount;
        if (hiddenCount) left += L"   ·   " + WithCommas(hiddenCount) + L" hidden (Ctrl+H)";
    }
    g_cv.Text(left, g_fSmall, R(14, top, g_W * 0.6f, g_H), g_statusMsg.empty() ? g_theme.subtext : g_theme.text);
    std::wstring hints = L"Ctrl+K search   ·   Ctrl+L path   ·   F2 rename   ·   Ctrl+T new tab";
    g_cv.Text(hints, g_fSmallRight, R(g_W * 0.4f, top, g_W - 14, g_H), Alpha(g_theme.subtext, 0.75f));
}

static void Paint() {
    if (!EnsureDevice()) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_W = Px2Dip(rc.right);
    g_H = Px2Dip(rc.bottom);
    g_zones.clear();
    g_rt->BeginDraw();
    g_rt->Clear(g_theme.backdrop ? D2D1::ColorF(0, 0, 0, 0) : g_theme.bottomBg);
    if (!g_tabs.empty()) {
        PaintToolbar();
        PaintSidebar();
        PaintList();
        PaintStatus();
        PaintTitleBar();   // last, so the dragged tab floats over everything
    }
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { DiscardDevice(); Redraw(); }
}

// ============================================================ hit testing
static Zone HitZoneTop(float x, float y) {
    // prefer the most specific zone (later zones are drawn on top, but tabs/close and dividers need priority)
    Zone best{ZNone, -1, {}};
    for (auto& z : g_zones) {
        if (!In(z.rc, x, y)) continue;
        if (z.kind == ZTabClose || z.kind == ZHeaderDiv || z.kind == ZSideSplitter || z.kind == ZScrollbar || z.kind == ZSearchClear) return z;
        best = z;
    }
    return best;
}

// ============================================================ menus
static void PinColorMenu(int pinIdx, POINT pt) {
    struct C { const wchar_t* name; uint32_t rgb; } colors[] = {
        {L"No color", 0}, {L"Red", 0xE5484D}, {L"Orange", 0xF5A524}, {L"Yellow", 0xF2D13A}, {L"Green", 0x30A46C},
        {L"Blue", 0x3E8EF7}, {L"Purple", 0x8E4EC6}, {L"Pink", 0xE93D82}};
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < (int)std::size(colors); ++i)
        AppendMenuW(m, MF_STRING | (g_config.pins[pinIdx].color == colors[i].rgb ? MF_CHECKED : 0), 100 + i, colors[i].name);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 100 && cmd < 100 + std::size(colors)) {
        g_config.pins[pinIdx].color = colors[cmd - 100].rgb;
        ConfigSave();
        Redraw();
    }
}

static void SidebarMenu(ZoneKind k, int idx, POINT pt) {
    std::wstring path;
    if (k == ZSidePin) path = g_config.pins[idx].path;
    else if (k == ZSideDrive) path = g_drives[idx].root;
    else return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open");
    AppendMenuW(m, MF_STRING, 2, L"Open in new tab");
    AppendMenuW(m, MF_STRING, 3, L"Open terminal here");
    AppendMenuW(m, MF_STRING, 4, L"Copy path");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (k == ZSidePin) {
        AppendMenuW(m, MF_STRING, 10, L"Color tag…");
        AppendMenuW(m, MF_STRING | (idx == 0 ? MF_GRAYED : 0), 11, L"Move up");
        AppendMenuW(m, MF_STRING | (idx + 1 == (int)g_config.pins.size() ? MF_GRAYED : 0), 12, L"Move down");
        AppendMenuW(m, MF_STRING, 13, L"Unpin");
    } else {
        AppendMenuW(m, MF_STRING, 20, L"Properties");
    }
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case 1: Navigate(T(), path); break;
        case 2: OpenFolderInNewTab(path, true); break;
        case 3: OpenTerminal(path); break;
        case 4: CopyPathsToClipboard({path}); break;
        case 10: PinColorMenu(idx, pt); break;
        case 11: std::swap(g_config.pins[idx], g_config.pins[idx - 1]); ConfigSave(); break;
        case 12: std::swap(g_config.pins[idx], g_config.pins[idx + 1]); ConfigSave(); break;
        case 13: g_config.pins.erase(g_config.pins.begin() + idx); ConfigSave(); break;
        case 20: ShowProperties(g_hwnd, {path}); break;
    }
    Redraw();
}

static void TabMenu(int idx, POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"New tab\tCtrl+T");
    AppendMenuW(m, MF_STRING, 2, L"Duplicate tab");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Close tab\tCtrl+W");
    AppendMenuW(m, MF_STRING | (g_tabs.size() > 1 ? 0 : MF_GRAYED), 4, L"Close other tabs");
    AppendMenuW(m, MF_STRING | (idx + 1 < (int)g_tabs.size() ? 0 : MF_GRAYED), 5, L"Close tabs to the right");
    AppendMenuW(m, MF_STRING | (g_closedTabs.empty() ? MF_GRAYED : 0), 6, L"Reopen closed tab\tCtrl+Shift+T");
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case 1: AddTab(g_tabs[idx].path, true, idx + 1); break;
        case 2: {
            Tab copy = g_tabs[idx];
            int at = AddTab(copy.path, true, idx + 1);
            g_tabs[at].back = copy.back;
            g_tabs[at].fwd = copy.fwd;
            break;
        }
        case 3: CloseTab(idx); break;
        case 4: {
            Tab keep = std::move(g_tabs[idx]);
            for (int i = 0; i < (int)g_tabs.size(); ++i) if (i != idx) g_closedTabs.push_back(g_tabs[i].path);
            g_tabs.clear();
            g_tabs.push_back(std::move(keep));
            g_active = 0;
            ActivateTab(0);
            break;
        }
        case 5:
            for (int i = (int)g_tabs.size() - 1; i > idx; --i) { g_closedTabs.push_back(g_tabs[i].path); g_tabs.erase(g_tabs.begin() + i); }
            if (g_active > idx) g_active = idx;
            ActivateTab(g_active);
            break;
        case 6: ReopenClosedTab(); break;
    }
}

static void AppMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    HMENU theme = CreatePopupMenu(), back = CreatePopupMenu(), density = CreatePopupMenu();
    for (int i = 0; i < kThemePresetCount; ++i)
        AppendMenuW(theme, MF_STRING | (g_config.theme == kThemePresets[i].id ? MF_CHECKED : 0), 100 + i, kThemePresets[i].name);
    const wchar_t* backs[] = {L"solid", L"mica", L"acrylic"};
    const wchar_t* backNames[] = {L"Solid", L"Mica", L"Acrylic (blur)"};
    for (int i = 0; i < 3; ++i)
        AppendMenuW(back, MF_STRING | (g_config.backdrop == backs[i] ? MF_CHECKED : 0) | (i && !SystemBackdropSupported() ? MF_GRAYED : 0),
                    200 + i, backNames[i]);
    AppendMenuW(density, MF_STRING | (g_config.density == L"compact" ? MF_CHECKED : 0), 300, L"Compact");
    AppendMenuW(density, MF_STRING | (g_config.density == L"comfortable" ? MF_CHECKED : 0), 301, L"Comfortable");

    AppendMenuW(m, MF_STRING, 1, L"New tab\tCtrl+T");
    AppendMenuW(m, MF_STRING | (g_closedTabs.empty() ? MF_GRAYED : 0), 2, L"Reopen closed tab\tCtrl+Shift+T");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_config.showHidden ? MF_CHECKED : 0), 10, L"Show hidden items\tCtrl+H");
    AppendMenuW(m, MF_STRING | (g_config.showExtensions ? MF_CHECKED : 0), 11, L"Show file extensions");
    AppendMenuW(m, MF_STRING | (g_config.foldersFirst ? MF_CHECKED : 0), 12, L"Folders first");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)theme, L"Theme");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)back, L"Background");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)density, L"Row height");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_config.restoreSession ? MF_CHECKED : 0), 20, L"Reopen my tabs on startup");
    AppendMenuW(m, MF_STRING, 21, L"Open settings && log folder");
    AppendMenuW(m, MF_STRING, 22, L"About Strata");
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 100 && cmd < 100 + (UINT)kThemePresetCount) { g_config.theme = kThemePresets[cmd - 100].id; ConfigSave(); ApplyTheme(); return; }
    if (cmd >= 200 && cmd < 203) { g_config.backdrop = backs[cmd - 200]; ConfigSave(); ApplyTheme(); return; }
    switch (cmd) {
        case 1: AddTab(T().path, true, g_active + 1); break;
        case 2: ReopenClosedTab(); break;
        case 10: g_config.showHidden = !g_config.showHidden; ConfigSave(); for (auto& t : g_tabs) BuildView(t); break;
        case 11: g_config.showExtensions = !g_config.showExtensions; ConfigSave(); break;
        case 12: g_config.foldersFirst = !g_config.foldersFirst; ConfigSave(); for (auto& t : g_tabs) BuildView(t); break;
        case 20: g_config.restoreSession = !g_config.restoreSession; ConfigSave(); break;
        case 21: Navigate(T(), DataDir()); break;
        case 22:
            MessageBoxW(g_hwnd,
                        L"Strata " STRATA_VERSION L"\n\nA fast, keyboard-friendly file manager for Windows.\n"
                        L"Part of the larosa023 suite.\n\ngithub.com/larosa023/Strata",
                        L"About Strata", MB_ICONINFORMATION);
            break;
        case 300: g_config.density = L"compact"; ConfigSave(); break;
        case 301: g_config.density = L"comfortable"; ConfigSave(); break;
    }
    Redraw();
}

static void HeaderMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    for (int c = 1; c < ColCount; ++c)
        AppendMenuW(m, MF_STRING | (g_config.columnVisible[c] ? MF_CHECKED : 0), 1 + c, kColLabel[c]);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 50, L"Reset column widths");
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 2 && cmd < 1 + ColCount) g_config.columnVisible[cmd - 1] = !g_config.columnVisible[cmd - 1];
    if (cmd == 50) { Config d; for (int c = 0; c < ColCount; ++c) g_config.columnWidth[c] = d.columnWidth[c]; }
    if (cmd) { ConfigSave(); Redraw(); }
}

static void CrumbMenu(int crumbIdx, POINT pt) {
    // children of the crumb's folder; the next crumb in the path is checked
    const Crumb& c = g_crumbs[crumbIdx];
    std::vector<std::wstring> paths, labels;
    if (c.path.empty()) {
        for (auto& d : g_drives) { paths.push_back(d.root); labels.push_back(DriveDisplayName(d)); }
    } else {
        for (auto& name : ListSubfolders(c.path, 400, g_config.showHidden)) { paths.push_back(PathJoin(c.path, name)); labels.push_back(name); }
    }
    std::wstring next = crumbIdx + 1 < (int)g_crumbs.size() ? g_crumbs[crumbIdx + 1].path : L"";
    HMENU m = CreatePopupMenu();
    if (paths.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No subfolders");
    for (size_t i = 0; i < paths.size(); ++i) {
        std::wstring label = labels[i];
        for (size_t k = 0; k < label.size(); ++k) if (label[k] == L'&') { label.insert(k, L"&"); ++k; }
        AppendMenuW(m, MF_STRING | (!next.empty() && _wcsicmp(next.c_str(), paths[i].c_str()) == 0 ? MF_CHECKED : 0) |
                           ((i > 0 && i % 30 == 0) ? MF_MENUBARBREAK : 0), 1 + i, label.c_str());
    }
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1 && cmd <= paths.size()) {
        if (GetKeyState(VK_CONTROL) < 0) OpenFolderInNewTab(paths[cmd - 1], true);
        else Navigate(T(), paths[cmd - 1]);
    }
}

static void OverflowMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < g_hiddenCrumbs.size(); ++i) {
        std::wstring p = g_hiddenCrumbs[i];
        std::wstring label = p == L"\x01" ? L"This PC" : (p.size() == 3 && g_driveNames.count(p) ? g_driveNames[p] : PathLeaf(p));
        AppendMenuW(m, MF_STRING, 1 + i, label.c_str());
    }
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1 && cmd <= g_hiddenCrumbs.size()) {
        std::wstring p = g_hiddenCrumbs[cmd - 1];
        Navigate(T(), p == L"\x01" ? L"" : p);
    }
}

enum ItemCmd { IC_OpenLocation = 20, IC_OpenTab = 1, IC_Pin, IC_CopyPath, IC_Terminal, IC_NewFolder, IC_NewText, IC_Paste, IC_Refresh, IC_Props,
               IC_SelectAll, IC_OpenWith };

static void ListContextMenu(POINT screenPt, bool onItem) {
    Tab& t = T();
    std::vector<std::wstring> paths = onItem ? SelectedPaths(t) : std::vector<std::wstring>{};
    std::vector<MenuEntry> extra;
    if (onItem && !paths.empty() && t.searchActive) {
        // Search results can come from many folders; the Windows menu works on items that share a folder
        std::wstring parent = PathParent(paths[0]);
        if (t.focus >= 0 && t.focus < (int)t.view.size()) parent = PathParent(t.items[t.view[t.focus]].path);
        std::vector<std::wstring> same;
        for (auto& p : paths) if (_wcsicmp(PathParent(p).c_str(), parent.c_str()) == 0) same.push_back(p);
        if (same.size() != paths.size()) SetStatus(L"The menu applies to the selected items in " + PathLeaf(parent));
        paths = same;
        extra.push_back({IC_OpenLocation, L"Open file location"});
    }
    if (onItem && !paths.empty()) {
        bool singleDir = false;
        if (paths.size() == 1 && t.focus >= 0 && t.focus < (int)t.view.size()) singleDir = IsDirectory(paths[0]);
        if (singleDir) {
            extra.push_back({IC_OpenTab, L"Open in new tab"});
            extra.push_back({IC_Pin, IsPinned(paths[0]) ? L"Unpin from sidebar" : L"Pin to sidebar"});
            extra.push_back({IC_Terminal, L"Open terminal here"});
        }
        extra.push_back({IC_CopyPath, paths.size() > 1 ? L"Copy paths\tCtrl+Shift+C" : L"Copy path\tCtrl+Shift+C"});
        int result = ShowItemContextMenu(g_hwnd, screenPt, t.path, paths, extra, [&](const std::wstring& verb) {
            if (verb == L"rename") { BeginRename(); return true; }
            if ((verb == L"open" || verb == L"explore" || verb == L"opennewwindow" || verb == L"opennewprocess") && paths.size() == 1 &&
                IsDirectory(paths[0])) {
                if (verb == L"open" || verb == L"explore") Navigate(T(), paths[0]);
                else OpenFolderInNewTab(paths[0], true);
                return true;
            }
            if (verb == L"pintohome") { TogglePin(paths[0]); return true; }
            return false;
        });
        switch (result) {
            case IC_OpenLocation: Navigate(T(), PathParent(paths[0]), true, PathLeaf(paths[0])); break;
            case IC_OpenTab: OpenFolderInNewTab(paths[0], true); break;
            case IC_Pin: TogglePin(paths[0]); break;
            case IC_Terminal: OpenTerminal(paths[0]); break;
            case IC_CopyPath: CopyPathsToClipboard(paths); break;
        }
        Redraw();
        return;
    }
    // background menu
    bool real = !t.path.empty();
    HMENU m = CreatePopupMenu();
    HMENU newMenu = CreatePopupMenu();
    AppendMenuW(newMenu, MF_STRING, IC_NewFolder, L"Folder\tCtrl+Shift+N");
    AppendMenuW(newMenu, MF_STRING, IC_NewText, L"Text document");
    AppendMenuW(m, MF_POPUP | (real ? 0 : MF_GRAYED), (UINT_PTR)newMenu, L"New");
    AppendMenuW(m, MF_STRING | (real && ClipboardHasFiles() ? 0 : MF_GRAYED), IC_Paste, L"Paste\tCtrl+V");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IC_Refresh, L"Refresh\tF5");
    AppendMenuW(m, MF_STRING | (t.view.empty() ? MF_GRAYED : 0), IC_SelectAll, L"Select all\tCtrl+A");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (real ? 0 : MF_GRAYED), IC_Terminal, L"Open terminal here");
    AppendMenuW(m, MF_STRING | (real ? 0 : MF_GRAYED), IC_CopyPath, L"Copy folder path");
    AppendMenuW(m, MF_STRING | (real ? 0 : MF_GRAYED), IC_Pin, real && IsPinned(t.path) ? L"Unpin from sidebar" : L"Pin to sidebar");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (real ? 0 : MF_GRAYED), IC_Props, L"Properties");
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case IC_NewFolder: NewFolder(); break;
        case IC_NewText: NewTextFile(); break;
        case IC_Paste: DoPaste(t.path); break;
        case IC_Refresh: Load(t, true); break;
        case IC_SelectAll: for (int v : t.view) t.sel[v] = 1; break;
        case IC_Terminal: OpenTerminal(t.path); break;
        case IC_CopyPath: CopyPathsToClipboard({t.path}); break;
        case IC_Pin: TogglePin(t.path); break;
        case IC_Props: ShowProperties(g_hwnd, {t.path}); break;
    }
    Redraw();
}

// ============================================================ focus + caret
static void SetFocusTo(Focus f) {
    if (g_focus == Focus::Rename && f != Focus::Rename) CommitRename(true);
    g_focus = f;
    bool caret = f != Focus::List;
    if (caret && !g_caretTimer) { SetTimer(g_hwnd, TIMER_CARET, GetCaretBlinkTime(), nullptr); g_caretTimer = true; }
    if (!caret && g_caretTimer) { KillTimer(g_hwnd, TIMER_CARET); g_caretTimer = false; }
    g_pathInput.caretOn = T().filter.caretOn = g_renameInput.caretOn = true;
    Redraw();
}

static void BeginPathEdit() {
    g_pathInput.SetText(T().path.empty() ? L"This PC" : T().path);
    g_pathInput.SelectAll();
    SetFocusTo(Focus::Path);
}

static void FocusSearch() {
    T().filter.SelectAll();
    SetFocusTo(Focus::Search);
}

static void OnFilterChanged() {
    Tab& t = T();
    bool empty = t.filter.text.find_first_not_of(L' ') == std::wstring::npos;
    if (t.path.empty() || t.searchActive) {
        // This PC searches every drive; an active subfolder search follows the text too
        if (empty) EndSearch(t);
        else SetTimer(g_hwnd, TIMER_SEARCH, t.path.empty() ? 250 : 350, nullptr);
        Redraw();
        return;
    }
    BuildView(t);
    t.scroll = t.scrollTarget = 0;
    if (!t.view.empty() && !t.filter.text.empty()) { t.focus = 0; }
    Redraw();
}

// ============================================================ keyboard
static void MoveFocus(Tab& t, int to, bool shift, bool ctrl) {
    if (t.view.empty()) return;
    to = std::clamp(to, 0, (int)t.view.size() - 1);
    if (shift) {
        if (t.anchor < 0) t.anchor = t.focus < 0 ? to : t.focus;
        SelectRange(t, t.anchor, to, ctrl);
        t.focus = to;
    } else if (ctrl) {
        t.focus = to;
    } else {
        SelectOnly(t, to);
    }
    EnsureVisible(t, to);
    Redraw();
}

static void TypeAhead(wchar_t ch) {
    Tab& t = T();
    DWORD now = GetTickCount();
    if (now - g_typeTime > 900) g_typeBuf.clear();
    g_typeTime = now;
    if (ch == L' ' && g_typeBuf.empty()) return;   // Space is reserved for quick preview
    std::wstring prev = g_typeBuf;
    g_typeBuf += (wchar_t)towlower(ch);
    bool repeat = g_typeBuf.size() > 1 && g_typeBuf.find_first_not_of(g_typeBuf[0]) == std::wstring::npos;
    std::wstring needle = repeat ? g_typeBuf.substr(0, 1) : g_typeBuf;
    int n = (int)t.view.size();
    if (!n) return;
    int start = t.focus < 0 ? 0 : (repeat || g_typeBuf.size() == 1 ? t.focus + 1 : t.focus);
    for (int k = 0; k < n; ++k) {
        int v = (start + k) % n;
        std::wstring nm = ToLower(DisplayName(t.items[t.view[v]]));
        if (nm.compare(0, needle.size(), needle) == 0) { MoveFocus(t, v, false, false); return; }
    }
    if (!prev.empty() && !repeat) g_typeBuf = prev;   // no match: keep the old prefix
}

static bool GlobalShortcut(UINT vk, bool ctrl, bool shift, bool alt) {
    Tab& t = T();
    if (ctrl && !alt) {
        switch (vk) {
            case 'T': if (shift) ReopenClosedTab(); else AddTab(t.path, true, g_active + 1); return true;
            case 'W': case VK_F4: CloseTab(g_active); return true;
            case VK_TAB: ActivateTab((g_active + (shift ? -1 : 1) + (int)g_tabs.size()) % (int)g_tabs.size()); return true;
            case VK_PRIOR: ActivateTab((g_active - 1 + (int)g_tabs.size()) % (int)g_tabs.size()); return true;
            case VK_NEXT: ActivateTab((g_active + 1) % (int)g_tabs.size()); return true;
            case 'L': BeginPathEdit(); return true;
            case 'K': case 'F': case 'E': FocusSearch(); return true;
            case 'R': Load(t, true); return true;
            case 'H': g_config.showHidden = !g_config.showHidden; ConfigSave(); for (auto& tb : g_tabs) BuildView(tb);
                      SetStatus(g_config.showHidden ? L"Showing hidden items" : L"Hiding hidden items"); return true;
            default: break;
        }
        if (vk >= '1' && vk <= '9') {
            int i = vk == '9' ? (int)g_tabs.size() - 1 : (int)(vk - '1');
            if (i < (int)g_tabs.size()) ActivateTab(i);
            return true;
        }
    }
    if (alt && !ctrl) {
        switch (vk) {
            case VK_LEFT: GoBack(t); return true;
            case VK_RIGHT: GoForward(t); return true;
            case VK_UP: GoUp(t); return true;
            case 'D': BeginPathEdit(); return true;
            default: break;
        }
    }
    if (vk == VK_BROWSER_BACK) { GoBack(t); return true; }
    if (vk == VK_BROWSER_FORWARD) { GoForward(t); return true; }
    if (vk == VK_F5 && !ctrl && !alt) { Load(t, true); return true; }
    return false;
}

static bool OnInputKey(UINT vk) {
    TextInput* in = g_focus == Focus::Search ? &T().filter : g_focus == Focus::Path ? &g_pathInput : &g_renameInput;
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (vk == VK_ESCAPE) {
        if (g_focus == Focus::Search) {
            if (!T().filter.text.empty()) { T().filter.SetText(L""); OnFilterChanged(); }
            SetFocusTo(Focus::List);
        } else if (g_focus == Focus::Path) {
            SetFocusTo(Focus::List);
        } else {
            CommitRename(false);
        }
        return true;
    }
    if (vk == VK_RETURN) {
        if (g_focus == Focus::Path) { std::wstring s = g_pathInput.text; SetFocusTo(Focus::List); NavigateTyped(s); }
        else if (g_focus == Focus::Rename) CommitRename(true);
        else {
            Tab& t = T();
            bool hasText = t.filter.text.find_first_not_of(L' ') != std::wstring::npos;
            bool pending = KillTimer(g_hwnd, TIMER_SEARCH) != FALSE;
            if (hasText && !ctrl && (pending || (!t.searchActive && !t.path.empty()))) {
                StartSearch(t, t.filter.text);   // search now (in a folder: include subfolders)
                return true;
            }
            SetFocusTo(Focus::List);
            if (!t.view.empty()) { if (t.focus < 0) t.focus = 0; SelectOnly(t, t.focus); if (ctrl) OpenSelection(t, false); }
        }
        return true;
    }
    if (g_focus == Focus::Search && (vk == VK_DOWN || vk == VK_TAB)) {
        Tab& t = T();
        SetFocusTo(Focus::List);
        if (!t.view.empty()) MoveFocus(t, std::max(0, t.focus), false, false);
        return true;
    }
    if (g_focus == Focus::Rename && vk == VK_TAB) { CommitRename(true); return true; }
    bool handled = false;
    bool changed = in->OnKey(g_hwnd, vk, handled);
    if (changed && g_focus == Focus::Search) OnFilterChanged();
    if (handled) { Redraw(); return true; }
    return false;
}

static void OnKeyDown(UINT vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (g_focus != Focus::List) {
        if (OnInputKey(vk)) return;
        if (g_focus == Focus::Rename) return;   // keep keys inside the rename box
        GlobalShortcut(vk, ctrl, shift, alt);
        return;
    }
    if (GlobalShortcut(vk, ctrl, shift, alt)) return;
    Tab& t = T();
    int page = std::max(1, (int)(ListViewH() / RowH()) - 1);
    int cur = t.focus < 0 ? -1 : t.focus;
    switch (vk) {
        case VK_DOWN: MoveFocus(t, cur + 1, shift, ctrl); return;
        case VK_UP: MoveFocus(t, cur < 0 ? 0 : cur - 1, shift, ctrl); return;
        case VK_NEXT: MoveFocus(t, cur + page, shift, ctrl); return;
        case VK_PRIOR: MoveFocus(t, cur - page, shift, ctrl); return;
        case VK_HOME: MoveFocus(t, 0, shift, ctrl); return;
        case VK_END: MoveFocus(t, (int)t.view.size() - 1, shift, ctrl); return;
        case VK_SPACE:
            if (ctrl && t.focus >= 0) { t.sel[t.view[t.focus]] ^= 1; Redraw(); }
            return;
        case VK_RETURN: if (alt) ShowProperties(g_hwnd, SelectedPaths(t)); else OpenSelection(t, ctrl); return;
        case VK_BACK: GoUp(t); return;
        case VK_F2: BeginRename(); return;
        case VK_DELETE: DoDelete(shift); return;
        case VK_ESCAPE:
            if (!t.filter.text.empty()) { t.filter.SetText(L""); OnFilterChanged(); }
            else { std::fill(t.sel.begin(), t.sel.end(), 0); Redraw(); }
            return;
        case VK_APPS: {
            POINT pt{0, 0};
            if (t.focus >= 0) {
                float y = g_rcList.top + (t.focus + 1) * RowH() - t.scroll;
                pt = {(LONG)((g_rcList.left + 60) * g_scale), (LONG)(y * g_scale)};
            } else {
                pt = {(LONG)((g_rcList.left + 60) * g_scale), (LONG)((g_rcList.top + 20) * g_scale)};
            }
            ClientToScreen(g_hwnd, &pt);
            ListContextMenu(pt, SelectedCount(t) > 0);
            return;
        }
        default: break;
    }
    if (ctrl && !alt) {
        switch (vk) {
            case 'A': for (int v : t.view) t.sel[v] = 1; Redraw(); return;
            case 'C': if (shift) CopyPathsToClipboard(SelectedPaths(t)); else DoCopyCut(false); return;
            case 'X': DoCopyCut(true); return;
            case 'V': DoPaste(t.path); return;
            case 'N': if (shift) NewFolder(); return;
            default: break;
        }
    }
    if (shift && vk == VK_F10) { OnKeyDown(VK_APPS); return; }
}

// ============================================================ mouse
static void UpdateHover(float x, float y) {
    Zone z = HitZoneTop(x, y);
    int row = (z.kind == ZList) ? RowAt(y, x) : -1;
    if (z.kind != g_hotKind || z.idx != g_hotIdx || row != g_hotRow) {
        g_hotKind = z.kind;
        g_hotIdx = z.idx;
        g_hotRow = row;
        Redraw();
    }
}

static void SetCursorFor(ZoneKind k) {
    LPCWSTR c = IDC_ARROW;
    if (k == ZSideSplitter || k == ZHeaderDiv) c = IDC_SIZEWE;
    else if (k == ZSearch || (k == ZPathBox && g_focus == Focus::Path)) c = IDC_IBEAM;
    SetCursor(LoadCursorW(nullptr, c));
}

static void ApplyMarquee() {
    Tab& t = T();
    float y0 = std::min(g_marqueeA.y, g_marqueeB.y), y1 = std::max(g_marqueeA.y, g_marqueeB.y);
    float rh = RowH();
    t.sel = g_marqueeBase;
    if (t.sel.size() != t.items.size()) t.sel.assign(t.items.size(), 0);
    int a = (int)std::floor(y0 / rh), b = (int)std::floor(y1 / rh);
    for (int v = std::max(0, a); v <= b && v < (int)t.view.size(); ++v) t.sel[t.view[v]] = 1;
    if (b >= 0 && b < (int)t.view.size()) t.focus = b;
}

static void OnLButtonDown(float x, float y, bool dbl) {
    SetCapture(g_hwnd);
    g_drag = Drag::None;
    g_dragMoved = false;
    g_dragStartX = x;
    g_dragStartY = y;
    Zone z = HitZoneTop(x, y);
    g_pressKind = z.kind;
    Tab& t = T();
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;

    // clicking anywhere outside an active text box ends editing
    if (g_focus == Focus::Rename && !In(g_rcRenameBox, x, y)) CommitRename(true);
    if (g_focus == Focus::Path && z.kind != ZPathBox && z.kind != ZCrumb && z.kind != ZCrumbArrow) SetFocusTo(Focus::List);
    if (g_focus == Focus::Search && z.kind != ZSearch && z.kind != ZSearchClear) SetFocusTo(Focus::List);

    switch (z.kind) {
        case ZTab:
            ActivateTab(z.idx);
            g_drag = Drag::Tab;
            g_dragIdx = z.idx;
            {
                float left, tabW;
                TabGeometry(left, tabW);
                g_dragOrig = x - (left + z.idx * tabW);
            }
            return;
        case ZTabClose: case ZNewTab: case ZCapMin: case ZCapMax: case ZCapClose: case ZBack: case ZFwd: case ZUp: case ZRefresh:
        case ZMenu: case ZSearchClear: case ZCrumb: case ZCrumbArrow: case ZCrumbOverflow: case ZSideThisPC: case ZSidePin: case ZSideDrive:
            g_drag = Drag::None;
            g_dragIdx = z.idx;
            return;   // acted on in button-up
        case ZPathBox:
            if (g_focus != Focus::Path) { BeginPathEdit(); return; }
            g_drag = Drag::InputSelect;
            g_dragInput = &g_pathInput;
            g_dragInputBox = R(g_rcPathBox.left + 12, g_rcPathBox.top, g_rcPathBox.right - 10, g_rcPathBox.bottom);
            break;
        case ZSearch:
            if (g_focus != Focus::Search) SetFocusTo(Focus::Search);
            g_drag = Drag::InputSelect;
            g_dragInput = &t.filter;
            g_dragInputBox = R(g_rcSearchBox.left + 32, g_rcSearchBox.top, g_rcSearchBox.right - 30, g_rcSearchBox.bottom);
            break;
        case ZSideSplitter:
            g_drag = Drag::Splitter;
            g_dragOrig = g_config.sidebarWidth;
            return;
        case ZHeaderDiv:
            g_drag = Drag::Column;
            g_dragIdx = z.idx;
            g_dragOrig = g_config.columnWidth[z.idx];
            return;
        case ZHeader:
            g_dragIdx = z.idx;
            return;   // sort on button-up
        case ZScrollbar: {
            D2D1_RECT_F thumb;
            if (!ScrollThumb(thumb)) return;
            if (!In(thumb, x, y)) {
                // page toward the click
                t.scrollTarget += (y < thumb.top ? -1 : 1) * ListViewH() * 0.9f;
                ClampScroll(t);
                SetTimer(g_hwnd, TIMER_SCROLL, 8, nullptr);
                return;
            }
            g_drag = Drag::Scrollbar;
            g_dragOrig = t.scroll;
            return;
        }
        case ZList: {
            if (g_focus != Focus::List && g_focus != Focus::Rename) SetFocusTo(Focus::List);
            if (g_focus == Focus::Rename) return;
            int v = RowAt(y, x);
            if (v >= 0) {
                int idx = t.view[v];
                if (dbl && !ctrl && !shift) { SelectOnly(t, v); OpenView(t, v, false); ReleaseCapture(); return; }
                if (shift) {
                    if (t.anchor < 0) t.anchor = t.focus < 0 ? v : t.focus;
                    SelectRange(t, t.anchor, v, ctrl);
                    t.focus = v;
                } else if (ctrl) {
                    t.sel[idx] ^= 1;
                    t.focus = t.anchor = v;
                } else if (!t.sel[idx]) {
                    SelectOnly(t, v);
                } else {
                    t.focus = v;
                    g_pressRow = v;   // clicking an already-selected row: collapse to it on button-up (unless dragged)
                    g_drag = Drag::ListPress;
                }
            } else {
                // empty area: start a marquee
                g_drag = Drag::Marquee;
                if (!ctrl && !shift) std::fill(t.sel.begin(), t.sel.end(), 0);
                g_marqueeBase = t.sel;
                g_marqueeA = g_marqueeB = D2D1::Point2F(x, y - g_rcList.top + t.scroll);
            }
            Redraw();
            return;
        }
        default:
            return;
    }
    if (g_drag == Drag::InputSelect && g_dragInput) {
        size_t pos = g_dragInput->HitTest(g_fInput, g_dragInputBox, x);
        if (dbl) g_dragInput->SelectAll();
        else if (shift) g_dragInput->caret = pos;
        else g_dragInput->Select(pos, pos);
        g_dragInput->caretOn = true;
        Redraw();
    }
}

static void OnMouseMove(float x, float y) {
    if (!g_tracking) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g_hwnd, 0};
        TrackMouseEvent(&tme);
        g_tracking = true;
    }
    if (std::fabs(x - g_dragStartX) > 4 || std::fabs(y - g_dragStartY) > 4) g_dragMoved = true;
    Tab& t = T();
    switch (g_drag) {
        case Drag::Tab: {
            if (!g_dragMoved) return;
            float left, tabW;
            TabGeometry(left, tabW);
            int target = std::clamp((int)std::floor((x - left) / tabW), 0, (int)g_tabs.size() - 1);
            if (target != g_dragIdx) {
                Tab moving = std::move(g_tabs[g_dragIdx]);
                g_tabs.erase(g_tabs.begin() + g_dragIdx);
                g_tabs.insert(g_tabs.begin() + target, std::move(moving));
                g_dragIdx = target;
                g_active = target;
            }
            Redraw();
            return;
        }
        case Drag::Splitter:
            g_config.sidebarWidth = std::clamp(g_dragOrig + (x - g_dragStartX), 150.0f, std::min(480.0f, g_W * 0.5f));
            Redraw();
            return;
        case Drag::Column:
            g_config.columnWidth[g_dragIdx] = std::clamp(g_dragOrig + (x - g_dragStartX), 56.0f, 1600.0f);
            Redraw();
            return;
        case Drag::Scrollbar: {
            D2D1_RECT_F tr = ScrollTrack(), thumb;
            if (!ScrollThumb(thumb)) return;
            float range = (tr.bottom - tr.top) - (thumb.bottom - thumb.top);
            if (range <= 0) return;
            t.scroll = t.scrollTarget = g_dragOrig + (y - g_dragStartY) / range * MaxScroll(t);
            ClampScroll(t);
            Redraw();
            return;
        }
        case Drag::Marquee:
            g_marqueeB = D2D1::Point2F(std::clamp(x, g_rcList.left, g_rcList.right), y - g_rcList.top + t.scroll);
            if (g_dragMoved) {
                ApplyMarquee();
                if (y < g_rcList.top + 10 || y > g_rcList.bottom - 10) SetTimer(g_hwnd, TIMER_MARQUEE, 30, nullptr);
                Redraw();
            }
            return;
        case Drag::InputSelect:
            if (g_dragInput) { g_dragInput->caret = g_dragInput->HitTest(g_fInput, g_dragInputBox, x); Redraw(); }
            return;
        default:
            break;
    }
    UpdateHover(x, y);
    SetCursorFor(g_hotKind);
}

static void OnLButtonUp(float x, float y) {
    Drag was = g_drag;
    g_drag = Drag::None;
    KillTimer(g_hwnd, TIMER_MARQUEE);
    if (GetCapture() == g_hwnd) ReleaseCapture();
    Tab& t = T();
    if (was == Drag::Splitter || was == Drag::Column) { ConfigSave(); Redraw(); return; }
    if (was == Drag::Tab || was == Drag::Scrollbar || was == Drag::Marquee || was == Drag::InputSelect) { Redraw(); return; }
    if (was == Drag::ListPress) {
        if (!g_dragMoved && g_pressRow >= 0 && g_pressRow < (int)t.view.size()) SelectOnly(t, g_pressRow);
        g_pressRow = -1;
        Redraw();
        return;
    }
    Zone z = HitZoneTop(x, y);
    if (z.kind != g_pressKind || z.idx != g_dragIdx) return;   // released somewhere else
    g_pressKind = ZNone;
    POINT pt{(LONG)(x * g_scale), (LONG)(y * g_scale)};
    ClientToScreen(g_hwnd, &pt);
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (z.kind) {
        case ZTabClose: CloseTab(z.idx); break;
        case ZNewTab: AddTab(t.path, true, g_active + 1); break;
        case ZCapMin: ShowWindow(g_hwnd, SW_MINIMIZE); break;
        case ZCapMax: ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case ZCapClose: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
        case ZBack: GoBack(t); break;
        case ZFwd: GoForward(t); break;
        case ZUp: GoUp(t); break;
        case ZRefresh: Load(t, true); break;
        case ZMenu: {
            POINT p{(LONG)(g_W * g_scale - 10 * g_scale), (LONG)((kTitleH + kToolH - 6) * g_scale)};
            ClientToScreen(g_hwnd, &p);
            AppMenu(p);
            break;
        }
        case ZSearchClear: t.filter.SetText(L""); OnFilterChanged(); SetFocusTo(Focus::Search); break;
        case ZCrumb: {
            const Crumb& c = g_crumbs[z.idx];
            if (ctrl) OpenFolderInNewTab(c.path, true);
            else if (_wcsicmp(c.path.c_str(), t.path.c_str()) != 0) Navigate(t, c.path);
            break;
        }
        case ZCrumbArrow: {
            POINT p{(LONG)(g_crumbs[z.idx].arrow.left * g_scale), (LONG)(g_rcPathBox.bottom * g_scale + 4)};
            ClientToScreen(g_hwnd, &p);
            CrumbMenu(z.idx, p);
            break;
        }
        case ZCrumbOverflow: {
            POINT p{(LONG)(g_crumbs[z.idx].rc.left * g_scale), (LONG)(g_rcPathBox.bottom * g_scale + 4)};
            ClientToScreen(g_hwnd, &p);
            OverflowMenu(p);
            break;
        }
        case ZSideThisPC: if (ctrl) OpenFolderInNewTab(L"", true); else Navigate(t, L""); break;
        case ZSidePin: if (ctrl) OpenFolderInNewTab(g_config.pins[z.idx].path, true); else Navigate(t, g_config.pins[z.idx].path); break;
        case ZSideDrive: if (ctrl) OpenFolderInNewTab(g_drives[z.idx].root, true); else Navigate(t, g_drives[z.idx].root); break;
        case ZHeader:
            if (g_config.sortColumn == z.idx) g_config.sortDescending = !g_config.sortDescending;
            else { g_config.sortColumn = z.idx; g_config.sortDescending = z.idx == ColModified || z.idx == ColSize; }
            ConfigSave();
            for (auto& tb : g_tabs) BuildView(tb);
            break;
        default: break;
    }
    Redraw();
}

static void OnMiddleClick(float x, float y) {
    Zone z = HitZoneTop(x, y);
    Tab& t = T();
    switch (z.kind) {
        case ZTab: case ZTabClose: CloseTab(z.idx); break;
        case ZSidePin: OpenFolderInNewTab(g_config.pins[z.idx].path, false); break;
        case ZSideDrive: OpenFolderInNewTab(g_drives[z.idx].root, false); break;
        case ZSideThisPC: OpenFolderInNewTab(L"", false); break;
        case ZCrumb: OpenFolderInNewTab(g_crumbs[z.idx].path, false); break;
        case ZList: {
            int v = RowAt(y, x);
            if (v >= 0 && t.items[t.view[v]].isDir()) OpenView(t, v, true);
            break;
        }
        default: break;
    }
    Redraw();
}

static void OnRightClick(float x, float y) {
    Zone z = HitZoneTop(x, y);
    POINT pt{(LONG)(x * g_scale), (LONG)(y * g_scale)};
    ClientToScreen(g_hwnd, &pt);
    Tab& t = T();
    switch (z.kind) {
        case ZTab: case ZTabClose: TabMenu(z.idx, pt); break;
        case ZSidePin: case ZSideDrive: SidebarMenu(z.kind, z.idx, pt); break;
        case ZHeader: case ZHeaderDiv: HeaderMenu(pt); break;
        case ZList: {
            if (g_focus != Focus::List) SetFocusTo(Focus::List);
            int v = RowAt(y, x);
            if (v >= 0) {
                if (!t.sel[t.view[v]]) SelectOnly(t, v);
                else t.focus = v;
                Redraw();
                ListContextMenu(pt, true);
            } else {
                std::fill(t.sel.begin(), t.sel.end(), 0);
                Redraw();
                ListContextMenu(pt, false);
            }
            break;
        }
        default: break;
    }
}

static void OnWheel(float x, float y, int delta) {
    Tab& t = T();
    if (In(g_rcSidebar, x, y)) {
        float maxS = std::max(0.0f, g_sideContentH - (g_rcSidebar.bottom - g_rcSidebar.top));
        g_sideScroll = std::clamp(g_sideScroll - delta / 120.0f * 48, 0.0f, maxS);
        Redraw();
        return;
    }
    if (y >= g_rcHeader.top) {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL || lines == 0) lines = 3;
        t.scrollTarget -= delta / 120.0f * RowH() * lines;
        ClampScroll(t);
        SetTimer(g_hwnd, TIMER_SCROLL, 8, nullptr);
    } else if (In(R(0, 0, g_W, kTitleH), x, y) && g_tabs.size() > 1) {
        ActivateTab((g_active + (delta > 0 ? -1 : 1) + (int)g_tabs.size()) % (int)g_tabs.size());
    }
}

// ============================================================ async results
static void OnListing(Listing* L) {
    for (auto& t : g_tabs) {
        if (t.id != L->tab || t.gen != L->gen) continue;
        t.items = std::move(L->items);
        t.types = std::move(L->types);
        t.error = L->error;
        t.ms = L->ms;
        t.loading = false;
        t.loaded = true;
        t.loadedAt = GetTickCount();
        t.sel.assign(t.items.size(), 0);
        t.view.clear();
        t.focus = -1;
        BuildView(t);
        // restore selection / focus by name
        if (!t.restoreSel.empty() || !t.restoreFocus.empty()) {
            for (int v = 0; v < (int)t.view.size(); ++v) {
                const std::wstring& nm = t.items[t.view[v]].name;
                if (t.restoreSel.count(nm)) t.sel[t.view[v]] = 1;
                if (nm == t.restoreFocus) t.focus = v;
            }
            t.restoreSel.clear();
            t.restoreFocus.clear();
        }
        if (!t.selectAfterLoad.empty()) {
            bool byPath = t.selectAfterLoad[0] == L'\x01';
            std::wstring want = byPath ? t.selectAfterLoad.substr(1) : t.selectAfterLoad;
            for (int v = 0; v < (int)t.view.size(); ++v) {
                const Item& it = t.items[t.view[v]];
                bool match = byPath ? _wcsicmp(it.path.c_str(), want.c_str()) == 0 : _wcsicmp(it.name.c_str(), want.c_str()) == 0;
                if (match) {
                    SelectOnly(t, v);
                    EnsureVisible(t, v);
                    break;
                }
            }
            bool rename = t.renameAfterLoad;
            t.selectAfterLoad.clear();
            t.renameAfterLoad = false;
            if (rename && &t == &T() && t.focus >= 0) BeginRename();
        }
        if (!t.restoreScroll) { t.scroll = t.scrollTarget = std::min(t.scrollTarget, MaxScroll(t)); }
        t.restoreScroll = false;
        if (t.focus < 0 && !t.view.empty()) t.focus = 0;
        ClampScroll(t);
        if (t.error) Log(L"Can't list %ls: %lu", t.path.c_str(), t.error);
        break;
    }
    delete L;
    Redraw();
}

static void OnSearch(SearchBatch* b) {
    for (auto& t : g_tabs) {
        if (t.id != b->tab || t.gen != b->gen || !t.searchActive) continue;
        std::map<std::wstring, int> idx;
        for (int i = 0; i < (int)t.types.size(); ++i) idx[t.types[i]] = i;
        std::vector<int> remap(b->typeNames.size());
        for (size_t i = 0; i < b->typeNames.size(); ++i) {
            auto it = idx.find(b->typeNames[i]);
            if (it == idx.end()) { remap[i] = (int)t.types.size(); t.types.push_back(b->typeNames[i]); }
            else remap[i] = it->second;
        }
        for (auto& it : b->items) {
            it.type = remap[it.type];
            t.items.push_back(std::move(it));
        }
        t.sel.resize(t.items.size(), 0);
        if (b->done) t.searchRunning = false;
        t.searchEverything = b->everything;
        t.searchCapped = b->capped;
        t.searchTotal = b->total;
        t.searchScanned = b->scannedFolders;
        BuildView(t);
        if (t.focus < 0 && !t.view.empty()) t.focus = 0;
        ClampScroll(t);
        break;
    }
    delete b;
    Redraw();
}

static void OnIcons(IconBatch* b) {
    for (auto& t : g_tabs) {
        if (t.id != b->tab || t.gen != b->gen) continue;
        for (auto& p : b->icons) if (p.first < (int)t.items.size()) t.items[p.first].icon = p.second;
        break;
    }
    delete b;
    Redraw();
}

static void OnDrives(std::vector<DriveInfo>* d) {
    bool changed = d->size() != g_drives.size();
    for (size_t i = 0; !changed && i < d->size(); ++i) changed = (*d)[i].root != g_drives[i].root || (*d)[i].ready != g_drives[i].ready;
    g_drives = std::move(*d);
    delete d;
    g_driveNames.clear();
    for (auto& dr : g_drives) g_driveNames[dr.root] = DriveDisplayName(dr);
    if (changed) for (auto& t : g_tabs) if (t.path.empty() && t.loaded) Load(t, true);
    Redraw();
}

static void OnOpDone(OpResult* r) {
    if (!r->ok && !r->aborted && r->hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) && r->hr != E_ABORT && r->hr != (HRESULT)0x80270000L /*COPYENGINE_E_USER_CANCELLED*/)
        SetStatus(L"The operation didn't complete (" + ErrorText(HRESULT_CODE(r->hr)) + L")");
    else if (r->ok) {
        const wchar_t* what = r->kind == OpKind::Copy ? L"Copied " : r->kind == OpKind::Move ? L"Moved " :
                              r->kind == OpKind::Recycle ? L"Moved to Recycle Bin: " : L"Deleted ";
        SetStatus(what + std::to_wstring(r->sources.size()) + (r->sources.size() == 1 ? L" item" : L" items"));
    }
    Tab& t = T();
    if (t.searchActive) {
        if (r->ok && r->kind != OpKind::Copy) {
            std::set<std::wstring> gone;
            for (auto& s : r->sources) gone.insert(ToLower(s));
            std::vector<Item> keep;
            for (auto& it : t.items) if (!gone.count(ToLower(it.path))) keep.push_back(std::move(it));
            t.items = std::move(keep);
            t.sel.assign(t.items.size(), 0);
            BuildView(t);
        }
    } else if (!t.path.empty()) {
        Load(t, true);
    }
    delete r;
    ListDrivesAsync(g_hwnd);   // free space changed
}

// ============================================================ window chrome
static int FrameThickness() {
    return GetSystemMetricsForDpi(SM_CXFRAME, g_dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g_dpi);
}

static LRESULT HitTestNC(HWND hwnd, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd, &pt);
    RECT rc;
    GetClientRect(hwnd, &rc);
    bool maxed = IsZoomed(hwnd) != FALSE;
    int b = (int)(kResize * g_scale);
    if (!maxed) {
        bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, bo = pt.y >= rc.bottom - b;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (bo && l) return HTBOTTOMLEFT;
        if (bo && r) return HTBOTTOMRIGHT;
        if (t) return HTTOP;
        if (bo) return HTBOTTOM;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
    }
    float x = Px2Dip(pt.x), y = Px2Dip(pt.y);
    if (y < kTitleH) {
        if (x >= g_W - 2 * kCapW && x < g_W - kCapW && y < kTitleH - 4) return HTMAXBUTTON;   // Windows 11 snap layouts
        Zone z = HitZoneTop(x, y);
        if (z.kind == ZNone) return HTCAPTION;
        return HTCLIENT;
    }
    return HTCLIENT;
}

static void SaveSession() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(g_hwnd, &wp)) {
        g_config.windowRect = wp.rcNormalPosition;
        g_config.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
    g_config.sessionTabs.clear();
    for (auto& t : g_tabs) g_config.sessionTabs.push_back(t.path);
    g_config.sessionActive = g_active;
    ConfigSave();
}

// ============================================================ window procedure
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT menuResult = 0;
    if (ShellMenuHandleMessage(msg, wp, lp, &menuResult)) return menuResult;
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                if (IsZoomed(hwnd)) {
                    int f = FrameThickness();
                    p->rgrc[0].left += f;
                    p->rgrc[0].top += f;
                    p->rgrc[0].right -= f;
                    p->rgrc[0].bottom -= f;
                    // leave a pixel for an auto-hide taskbar
                    APPBARDATA abd{sizeof(abd)};
                    if (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) p->rgrc[0].bottom -= 1;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            LRESULT hit = HitTestNC(hwnd, lp);
            if (hit == HTMAXBUTTON && g_hotKind != ZCapMax) { g_hotKind = ZCapMax; g_hotIdx = -1; Redraw(); }
            else if (hit != HTMAXBUTTON && g_hotKind == ZCapMax) { g_hotKind = ZNone; Redraw(); }
            return hit;
        }
        case WM_NCMOUSEMOVE:
            if (wp == HTMAXBUTTON) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            break;
        case WM_NCMOUSELEAVE:
            if (g_hotKind == ZCapMax) { g_hotKind = ZNone; Redraw(); }
            break;
        case WM_NCLBUTTONDOWN:
            if (wp == HTMAXBUTTON) return 0;
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON) { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
            break;
        case WM_NCACTIVATE:
            Redraw();
            return DefWindowProcW(hwnd, msg, wp, -1);
        case WM_ACTIVATE:
            Redraw();
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {(LONG)(720 * g_scale), (LONG)(440 * g_scale)};
            return 0;
        }
        case WM_SIZE:
            if (g_rt) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_rt->Resize(D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
            }
            Redraw();
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            g_scale = g_dpi / 96.0f;
            IconsInit(g_dpi >= 120);
            if (g_rt) g_rt->SetDpi((float)g_dpi, (float)g_dpi);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Redraw();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { SetCursorFor(g_hotKind); return TRUE; }
            break;
        case WM_MOUSEMOVE: OnMouseMove(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_drag == Drag::None && (g_hotKind != ZNone || g_hotRow >= 0)) { g_hotKind = ZNone; g_hotIdx = -1; g_hotRow = -1; Redraw(); }
            return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp)), false); return 0;
        case WM_LBUTTONDBLCLK: {
            float x = Px2Dip(GET_X_LPARAM(lp)), y = Px2Dip(GET_Y_LPARAM(lp));
            if (y < kTitleH && HitZoneTop(x, y).kind == ZNone) { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
            OnLButtonDown(x, y, true);
            return 0;
        }
        case WM_LBUTTONUP: OnLButtonUp(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_MBUTTONUP: OnMiddleClick(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_RBUTTONUP: OnRightClick(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) GoBack(T());
            else GoForward(T());
            return TRUE;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            OnWheel(Px2Dip(pt.x), Px2Dip(pt.y), GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd && g_drag != Drag::None) {
                if (g_drag == Drag::Splitter || g_drag == Drag::Column) ConfigSave();
                g_drag = Drag::None;
                Redraw();
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            bool handledSys = msg == WM_SYSKEYDOWN && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == 'D' || wp == VK_RETURN);
            if (msg == WM_SYSKEYDOWN && !handledSys && wp != VK_F10) break;
            OnKeyDown((UINT)wp);
            return 0;
        }
        case WM_SYSCHAR:
            if (wp == L' ') break;   // Alt+Space system menu
            return 0;
        case WM_CHAR: {
            wchar_t ch = (wchar_t)wp;
            if (g_focus == Focus::List) {
                if (ch >= 0x20 && GetKeyState(VK_CONTROL) >= 0) TypeAhead(ch);
                return 0;
            }
            TextInput* in = g_focus == Focus::Search ? &T().filter : g_focus == Focus::Path ? &g_pathInput : &g_renameInput;
            if (in->OnChar(ch)) {
                if (g_focus == Focus::Search) OnFilterChanged();
                Redraw();
            }
            return 0;
        }
        case WM_TIMER:
            switch (wp) {
                case TIMER_SCROLL: {
                    Tab& t = T();
                    float d = t.scrollTarget - t.scroll;
                    if (std::fabs(d) < 0.5f) { t.scroll = t.scrollTarget; KillTimer(hwnd, TIMER_SCROLL); }
                    else t.scroll += d * 0.32f;
                    if (g_hwnd) {
                        POINT pt;
                        GetCursorPos(&pt);
                        ScreenToClient(hwnd, &pt);
                        g_hotRow = RowAt(Px2Dip(pt.y), Px2Dip(pt.x));
                    }
                    Redraw();
                    return 0;
                }
                case TIMER_CARET: {
                    if (g_focus == Focus::List) { KillTimer(hwnd, TIMER_CARET); g_caretTimer = false; return 0; }
                    TextInput* in = g_focus == Focus::Search ? &T().filter : g_focus == Focus::Path ? &g_pathInput : &g_renameInput;
                    in->caretOn = !in->caretOn;
                    Redraw();
                    return 0;
                }
                case TIMER_REFRESH:
                    KillTimer(hwnd, TIMER_REFRESH);
                    if (!g_tabs.empty() && g_focus != Focus::Rename && !T().searchActive) Load(T(), true);
                    return 0;
                case TIMER_SEARCH:
                    KillTimer(hwnd, TIMER_SEARCH);
                    if (!g_tabs.empty()) StartSearch(T(), T().filter.text);
                    return 0;
                case TIMER_STATUS:
                    KillTimer(hwnd, TIMER_STATUS);
                    g_statusMsg.clear();
                    Redraw();
                    return 0;
                case TIMER_DRIVES:
                    ListDrivesAsync(hwnd);
                    return 0;
                case TIMER_MARQUEE: {
                    if (g_drag != Drag::Marquee) { KillTimer(hwnd, TIMER_MARQUEE); return 0; }
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd, &pt);
                    float y = Px2Dip(pt.y);
                    Tab& t = T();
                    float step = 0;
                    if (y < g_rcList.top + 10) step = -(g_rcList.top + 10 - y) * 0.6f - 4;
                    else if (y > g_rcList.bottom - 10) step = (y - g_rcList.bottom + 10) * 0.6f + 4;
                    if (step == 0) { KillTimer(hwnd, TIMER_MARQUEE); return 0; }
                    t.scroll = t.scrollTarget = t.scroll + step;
                    ClampScroll(t);
                    g_marqueeB.y = std::clamp(y, g_rcList.top, g_rcList.bottom) - g_rcList.top + t.scroll;
                    ApplyMarquee();
                    Redraw();
                    return 0;
                }
            }
            break;
        case WM_STRATA_LISTING: OnListing(reinterpret_cast<Listing*>(lp)); return 0;
        case WM_STRATA_ICONS: OnIcons(reinterpret_cast<IconBatch*>(lp)); return 0;
        case WM_STRATA_DRIVES: OnDrives(reinterpret_cast<std::vector<DriveInfo>*>(lp)); return 0;
        case WM_STRATA_OPDONE: OnOpDone(reinterpret_cast<OpResult*>(lp)); return 0;
        case WM_STRATA_SEARCH: OnSearch(reinterpret_cast<SearchBatch*>(lp)); return 0;
        case WM_STRATA_DIRCHANGED:
            if ((uint64_t)wp == WatchGeneration()) SetTimer(hwnd, TIMER_REFRESH, 350, nullptr);
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) ListDrivesAsync(hwnd);
            return TRUE;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) ApplyTheme();
            break;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplyTheme();
            break;
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (SearchHandleCopyData(hwnd, cds)) return TRUE;
            if (cds && cds->dwData == 0x5354 /*'ST'*/) {
                std::wstring path(reinterpret_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
                while (!path.empty() && path.back() == 0) path.pop_back();
                if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                if (!path.empty()) MainOpenPath(path, true);
            }
            return TRUE;
        }
        case WM_CLOSE:
            if (g_focus == Focus::Rename) CommitRename(true);
            SaveSession();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            WatchFolder(hwnd, L"");
            DiscardDevice();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================ public
void MainOpenPath(const std::wstring& raw, bool newTab) {
    std::wstring p = NormalizeFolder(raw);
    if (!p.empty() && !IsDirectory(p)) {
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring parent = PathParent(p);
            int i = newTab ? AddTab(parent, true, (int)g_tabs.size()) : g_active;
            if (!newTab) Navigate(g_tabs[i], parent);
            g_tabs[i].selectAfterLoad = PathLeaf(p);
            return;
        }
        p.clear();
    }
    if (newTab) AddTab(p, true, (int)g_tabs.size());
    else Navigate(T(), p);
}

HWND MainCreate(HINSTANCE inst, const std::vector<std::wstring>& openPaths) {
    if (!gfx::Init()) { Log(L"Direct2D init failed"); return nullptr; }
    CreateFormats();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = STRATA_WNDCLASS;
    RegisterClassExW(&wc);

    // initial placement
    RECT r = g_config.windowRect;
    bool haveRect = r.right - r.left > 300 && r.bottom - r.top > 200;
    if (haveRect) {
        // make sure it's on a monitor
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (!mon) haveRect = false;
    }
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = CW_USEDEFAULT, h = CW_USEDEFAULT;
    if (haveRect) { x = r.left; y = r.top; w = r.right - r.left; h = r.bottom - r.top; }
    else {
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        float s = GetDpiForSystem() / 96.0f;
        w = std::min((int)(1280 * s), (int)(mi.rcWork.right - mi.rcWork.left) - 80);
        h = std::min((int)(800 * s), (int)(mi.rcWork.bottom - mi.rcWork.top) - 80);
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    }
    g_hwnd = CreateWindowExW(0, STRATA_WNDCLASS, L"Strata", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { Log(L"CreateWindow failed %lu", GetLastError()); return nullptr; }
    g_dpi = GetDpiForWindow(g_hwnd);
    g_scale = g_dpi / 96.0f;
    IconsInit(g_dpi >= 120);
    ApplyTheme();
    // re-run WM_NCCALCSIZE now that the custom frame is in place
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // tabs: restored session, then anything passed on the command line
    if (g_config.restoreSession)
        for (auto& p : g_config.sessionTabs) if (p.empty() || IsDirectory(p)) AddTab(p, false);
    int active = std::clamp(g_config.sessionActive, 0, std::max(0, (int)g_tabs.size() - 1));
    for (auto& p : openPaths) { MainOpenPath(p, true); active = (int)g_tabs.size() - 1; }
    if (g_tabs.empty()) AddTab(L"", false);
    g_active = std::clamp(active, 0, (int)g_tabs.size() - 1);
    WatchActive();
    ListDrivesAsync(g_hwnd);
    SetTimer(g_hwnd, TIMER_DRIVES, 60 * 1000, nullptr);

    ShowWindow(g_hwnd, g_config.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    return g_hwnd;
}
