// Strata - main window: title bar with tabs, toolbar, breadcrumb, sidebar, file list, status bar
#include "window.h"
#include "config.h"
#include "gfx.h"
#include "fs.h"
#include "shell.h"
#include "textinput.h"
#include "search.h"
#include "tags.h"
#include "queue.h"
#include "sizes.h"
#include "dragdrop.h"
#include "autoupdate.h"
#include "preview.h"
#include "settings.h"
#include "system.h"
#include "index.h"
#include "query.h"
#include "batch.h"
#include <unordered_map>
#include <list>
#include <uxtheme.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <dbt.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace gfx;

// ============================================================ state
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct ColGeom { int col; float left, right; };
}  // namespace
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
enum ViewMode { ViewList = 0, ViewGrid = 1 };
}  // namespace

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Pane {
    int id = 0;                              // pane id (unique across tabs)
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
    bool searchActive = false, searchRunning = false, searchEverything = false, searchCapped = false, searchIndex = false;
    std::wstring searchQuery;
    uint64_t searchTotal = 0;
    size_t searchScanned = 0;
    std::wstring tagFilter;                  // showing every item with this tag (virtual view)
    std::wstring archive;                    // set when browsing inside an archive (read-only)
    int viewMode = ViewList;
    int sortColumn = ColName;
    bool sortDescending = false;
    uint64_t maxSize = 0;                    // biggest item (for size bars)
    // geometry from the last paint (DIPs)
    D2D1_RECT_F rcPane{}, rcList{}, rcHeader{};
    std::vector<ColGeom> cols;
    float rowRight = 1e9f;
    int gridCols = 1;
    float tileW = 120, tileH = 140;
};
}  // namespace

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Tab {
    int id = 0;
    Pane pane[2];
    int active = 0;                          // which pane has focus
};
}  // namespace

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
enum class Focus { List, Search, Path, Rename };
}  // namespace

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
enum ZoneKind {
    ZNone, ZTab, ZTabClose, ZNewTab, ZCapMin, ZCapMax, ZCapClose, ZBack, ZFwd, ZUp, ZRefresh, ZPathBox, ZCrumb, ZCrumbArrow,
    ZCrumbOverflow, ZSearch, ZSearchClear, ZMenu, ZSideThisPC, ZSidePin, ZSideDrive, ZSideSplitter, ZHeader, ZHeaderDiv,
    ZList, ZScrollbar, ZPaneSplitter, ZPreviewSplitter, ZViewSwitch, ZUpdatePill, ZQueueStatus, ZQueueJobPause, ZQueueJobCancel,
    ZQueueClear, ZQueueClose, ZPreviewButton, ZSideTag, ZSideSaved, ZSideRecent, ZSideSection, ZPreviewToggle
};
}  // namespace
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Zone { ZoneKind kind; int idx; D2D1_RECT_F rc; int pane = -1; };
}  // namespace

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
static std::vector<SessionTab> g_closedTabs;
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
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
enum class Drag { None, Tab, Splitter, Column, Scrollbar, Marquee, InputSelect, ListPress, PaneSplit, PreviewSplit };
}  // namespace
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
static D2D1_RECT_F g_rcSidebar, g_rcPathBox, g_rcSearchBox, g_rcRenameBox;
// the active pane's geometry
#define g_rcList (T().rcList)
#define g_rcHeader (T().rcHeader)
#define g_cols (T().cols)
#define g_rowRight (T().rowRight)
static float g_W = 0, g_H = 0;

// timers
constexpr UINT_PTR TIMER_SCROLL = 1, TIMER_CARET = 2, TIMER_REFRESH = 3, TIMER_STATUS = 4, TIMER_DRIVES = 5,
                   TIMER_MARQUEE = 6, TIMER_SEARCH = 7, TIMER_REFRESH1 = 8,
                   TIMER_PREVIEW = 11, TIMER_QUEUECLOSE = 13;

// metrics (DIPs)
constexpr float kTitleH = 40, kToolH = 48, kStatusH = 28, kHeaderH = 32, kCapW = 46, kSplitW = 6, kResize = 6;
static float RowH() {
    float base = g_config.density == L"compact" ? 26.0f : 32.0f;
    return base + std::max(0.0f, (g_config.fontSize - 13.0f) * 2.0f);
}

static Tab& TB() { return g_tabs[g_active]; }
static Pane& T() { return g_tabs[g_active].pane[g_tabs[g_active].active]; }
static Pane& OtherPane() { return g_tabs[g_active].pane[1 - g_tabs[g_active].active]; }
static bool DualMode() { return g_config.layout == L"dual"; }
// The This PC page lists drives (unless it's showing search results)
static bool DriveView(const Pane& t) { return t.path.empty() && !t.searchActive; }
static float Px2Dip(int v) { return v / g_scale; }
static void Redraw() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }

// forward declarations (features defined further down)
static void TogglePreview();
static void ToggleDual();
static void ToggleGrid();
static void QuickPreview();
static void UpdatePillClicked();
static void PreviewButton(int idx);
static void RunSavedSearch(int idx, bool newTab);
static void SaveCurrentSearch();
static void OpenSettings(int page);
static void PreviewScroll(int delta);
static void PreviewRefresh(bool force);
static void OnUpdateStatus();
static bool g_manualUpdateCheck = false;

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

// Windows "Text size" (Settings > Accessibility) scales Strata's text too
static float TextScale() {
    DWORD v = 100, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) v = 100;
    return std::clamp(v / 100.0f, 1.0f, 2.25f);
}

static void CreateFormats() {
    ReleaseFormats();
    static std::wstring family;
    family = g_config.fontFamily.empty() ? UiFont() : g_config.fontFamily;
    const wchar_t* ui = family.c_str();
    float k = g_config.fontSize / 13.0f * TextScale();
    g_fUi = Format(ui, 13 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fUiSemi = Format(ui, 13 * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fSmall = Format(ui, 11.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fSmallRight = Format(ui, 11.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    g_fRight = Format(ui, 12.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, true);
    g_fHeader = Format(ui, 12 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fIcon = Format(IconFont(), 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconSmall = Format(IconFont(), 10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconBig = Format(IconFont(), 36, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fInput = Format(ui, 13 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fTitle = Format(DisplayFont(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    g_fSection = Format(ui, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fEmpty = Format(DisplayFont(), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_fCenter = Format(ui, 12.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    if (g_fSection) g_fSection->SetIncrementalTabStop(40);
}

static void ThumbsDiscard();
static void PreviewBitmapsDiscard();
static void DiscardDevice() {
    IconsDiscard();
    ThumbsDiscard();
    PreviewBitmapsDiscard();
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

static std::wstring ItemPath(const Pane& t, const Item& it) {
    if (!it.path.empty()) return it.path;
    return PathJoin(t.path, it.name);
}

static bool MatchesFilter(const std::wstring& nameLower, const std::wstring& filterLower) {
    if (filterLower.empty()) return true;
    if (filterLower.find_first_of(L"*?") != std::wstring::npos) return PathMatchSpecW(nameLower.c_str(), filterLower.c_str()) != FALSE;
    return nameLower.find(filterLower) != std::wstring::npos;
}

static void BuildView(Pane& t) {
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
    int col = t.sortColumn;
    bool desc = t.sortDescending;
    bool thisPC = DriveView(t);
    bool search = t.searchActive || !t.tagFilter.empty();
    auto& items = t.items;
    auto& types = t.types;
    // sizes used for sorting and the size bars: folders use their calculated size when known
    std::vector<uint64_t> eff(items.size(), 0);
    uint64_t maxSize = 0;
    for (int i : t.view) {
        const Item& it = items[i];
        uint64_t v = it.size;
        if (it.isDir() && it.path.empty()) {
            FolderSize fs;
            if (SizesGet(PathJoin(t.path, it.name), fs) && fs.done) v = fs.bytes;
        }
        eff[i] = v;
        if (!thisPC) maxSize = std::max(maxSize, v);
    }
    t.maxSize = maxSize;
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
            case ColSize: c = eff[ia] < eff[ib] ? -1 : eff[ia] > eff[ib] ? 1 : 0; break;
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

static float ListViewH(const Pane& t) { return std::max(0.0f, t.rcList.bottom - t.rcList.top); }
static float ListViewH() { return ListViewH(T()); }
// grid view lays items out in rows of tiles
static float ItemRowH(const Pane& t) { return t.viewMode == ViewGrid ? t.tileH : RowH(); }
static int RowsOf(const Pane& t) {
    if (t.viewMode != ViewGrid) return (int)t.view.size();
    int c = std::max(1, t.gridCols);
    return ((int)t.view.size() + c - 1) / c;
}
static float ContentH(const Pane& t) { return RowsOf(t) * ItemRowH(t) + 8; }
static float MaxScroll(const Pane& t) { return std::max(0.0f, ContentH(t) - ListViewH(t)); }

static void ClampScroll(Pane& t) {
    float m = MaxScroll(t);
    t.scrollTarget = std::clamp(t.scrollTarget, 0.0f, m);
    t.scroll = std::clamp(t.scroll, 0.0f, m);
}

static void EnsureVisible(Pane& t, int v) {
    if (v < 0) return;
    float rh = ItemRowH(t), y = (t.viewMode == ViewGrid ? v / std::max(1, t.gridCols) : v) * rh, h = ListViewH(t);
    if (y < t.scrollTarget) t.scrollTarget = y;
    else if (y + rh > t.scrollTarget + h) t.scrollTarget = y + rh - h + 4;
    ClampScroll(t);
    t.scroll = t.scrollTarget;
}

static std::vector<std::wstring> SelectedPaths(const Pane& t) {
    std::vector<std::wstring> out;
    for (int v : t.view) if (t.sel[v]) out.push_back(ItemPath(t, t.items[v]));
    return out;
}

static int SelectedCount(const Pane& t) {
    int n = 0;
    for (int v : t.view) n += t.sel[v] ? 1 : 0;
    return n;
}

static void SelectOnly(Pane& t, int v) {
    std::fill(t.sel.begin(), t.sel.end(), 0);
    if (v >= 0 && v < (int)t.view.size()) t.sel[t.view[v]] = 1;
    t.focus = v;
    t.anchor = v;
}

static void SelectRange(Pane& t, int a, int b, bool add) {
    if (!add) std::fill(t.sel.begin(), t.sel.end(), 0);
    if (a > b) std::swap(a, b);
    for (int v = std::max(0, a); v <= b && v < (int)t.view.size(); ++v) t.sel[t.view[v]] = 1;
}

// ============================================================ navigation
static void WatchActive() {
    if (g_tabs.empty()) { WatchFolder(g_hwnd, 0, L""); WatchFolder(g_hwnd, 1, L""); return; }
    Tab& tb = TB();
    // watch both visible panes (only the active one in single-pane mode)
    for (int i = 0; i < 2; ++i) {
        const Pane& p = tb.pane[i];
        bool visible = DualMode() || i == tb.active;
        WatchFolder(g_hwnd, i, visible && !p.searchActive && p.tagFilter.empty() ? p.path : L"");
    }
}

static void StartSearch(Pane& t, const std::wstring& query);

// Each folder can remember its own sort order and list/grid view
static void ApplyFolderView(Pane& t) {
    FolderView v;
    if (g_config.rememberFolderViews && !t.path.empty() && FolderViewGet(t.path, v)) {
        t.sortColumn = v.sortColumn;
        t.sortDescending = v.sortDescending;
        t.viewMode = v.viewMode;
    } else {
        t.sortColumn = g_config.sortColumn;
        t.sortDescending = g_config.sortDescending;
        t.viewMode = ViewList;
    }
}

static void SaveFolderView(const Pane& t) {
    if (!g_config.rememberFolderViews || t.path.empty() || t.searchActive || !t.tagFilter.empty()) return;
    FolderViewSet(t.path, {t.sortColumn, t.sortDescending, t.viewMode});
}

static void ShowTag(Pane& t, const std::wstring& tagId);

static void Load(Pane& t, bool keepState) {
    if (t.searchActive) { StartSearch(t, t.searchQuery); return; }
    if (!t.tagFilter.empty()) { ShowTag(t, t.tagFilter); return; }
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

static void Navigate(Pane& t, const std::wstring& path, bool addHistory = true, const std::wstring& selectName = L"") {
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
    t.tagFilter.clear();
    if (&t == &T()) KillTimer(g_hwnd, TIMER_SEARCH);
    t.selectAfterLoad = selectName;
    t.renameAfterLoad = false;
    ApplyFolderView(t);
    if (!p.empty()) {
        auto& rec = g_config.recentFolders;
        rec.erase(std::remove_if(rec.begin(), rec.end(), [&](const std::wstring& r) { return _wcsicmp(r.c_str(), p.c_str()) == 0; }), rec.end());
        rec.insert(rec.begin(), p);
        if (rec.size() > 12) rec.resize(12);
    }
    Load(t, false);
    WatchActive();
    if (&t == &T() && (g_focus == Focus::Search || g_focus == Focus::Path || g_focus == Focus::Rename)) g_focus = Focus::List;
    Redraw();
}

// Show every file and folder carrying a tag (a "smart" view)
static void ShowTag(Pane& t, const std::wstring& tagId) {
    if (!t.path.empty() || !t.tagFilter.empty() || t.searchActive) {
        t.back.push_back(t.path);
        t.fwd.clear();
    }
    t.tagFilter = tagId;
    t.searchActive = t.searchRunning = false;
    t.filter.SetText(L"");
    t.gen = g_nextGen++;
    t.items.clear();
    t.types.clear();
    std::map<std::wstring, int> typeIdx;
    for (auto& path : TagsFind(tagId)) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) continue;
        Item it;
        it.name = PathLeaf(path);
        it.path = path;
        it.attrs = fa.dwFileAttributes;
        it.modified = fa.ftLastWriteTime;
        it.size = it.isDir() ? 0 : ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        std::wstring tn;
        TypeAndIcon(it.name, it.isDir(), tn, it.icon);
        auto ti = typeIdx.find(tn);
        if (ti == typeIdx.end()) { ti = typeIdx.emplace(tn, (int)t.types.size()).first; t.types.push_back(tn); }
        it.type = ti->second;
        t.items.push_back(std::move(it));
    }
    t.sel.assign(t.items.size(), 0);
    t.focus = t.anchor = -1;
    t.scroll = t.scrollTarget = 0;
    t.loaded = true;
    t.loading = false;
    t.error = 0;
    BuildView(t);
    if (!t.view.empty()) t.focus = 0;
    WatchActive();
    Redraw();
}

static void StartSearch(Pane& t, const std::wstring& query) {
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

static void EndSearch(Pane& t) {
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

static void GoBack(Pane& t) {
    if (t.back.empty()) return;
    std::wstring from = t.path;
    std::wstring to = t.back.back();
    t.back.pop_back();
    t.fwd.push_back(from);
    Navigate(t, to, false, ChildMarker(NormalizeFolder(to), from));
}

static void GoForward(Pane& t) {
    if (t.fwd.empty()) return;
    std::wstring to = t.fwd.back();
    t.fwd.pop_back();
    t.back.push_back(t.path);
    Navigate(t, to, false);
}

static void GoUp(Pane& t) {
    if (t.path.empty()) return;
    std::wstring child = t.path;
    std::wstring parent = PathParent(child);
    Navigate(t, parent, true, ChildMarker(parent, child));
}

static void ApplyFolderView(Pane& t);

// A tab holds two panes. `right` defaults to `left`.
static int AddTab(const std::wstring& left, bool activate, int insertAt = -1, const std::wstring* right = nullptr, int activePane = 0) {
    Tab t;
    t.id = g_nextTabId++;
    t.pane[0].id = g_nextTabId++;
    t.pane[1].id = g_nextTabId++;
    t.pane[0].path = NormalizeFolder(left);
    t.pane[1].path = NormalizeFolder(right ? *right : left);
    t.active = activePane & 1;
    if (insertAt < 0 || insertAt > (int)g_tabs.size()) insertAt = (int)g_tabs.size();
    g_tabs.insert(g_tabs.begin() + insertAt, std::move(t));
    if (activate) g_active = insertAt;
    else if (insertAt <= g_active && g_tabs.size() > 1) ++g_active;
    for (auto& p : g_tabs[insertAt].pane) { ApplyFolderView(p); Load(p, false); }
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
    for (auto& p : TB().pane)
        if (p.loaded && !p.loading && !p.searchActive && GetTickCount() - p.loadedAt > 1500) Load(p, true);
    WatchActive();
    if (g_focus == Focus::Search && T().filter.text.empty()) g_focus = Focus::List;
    Redraw();
}

static void SetActivePane(int i) {
    Tab& tb = TB();
    if (i == tb.active || i < 0 || i > 1) return;
    if (g_focus == Focus::Rename || g_focus == Focus::Path) g_focus = Focus::List;
    if (g_focus == Focus::Search) g_focus = Focus::List;
    tb.active = i;
    Redraw();
}

static SessionTab TabState(const Tab& t) { return {t.pane[0].path, t.pane[1].path, t.active}; }

static void CloseTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    g_closedTabs.push_back(TabState(g_tabs[i]));
    if (g_closedTabs.size() > 25) g_closedTabs.erase(g_closedTabs.begin());
    if (g_tabs.size() == 1) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }
    g_tabs.erase(g_tabs.begin() + i);
    if (g_active > i || g_active >= (int)g_tabs.size()) g_active = std::max(0, g_active - 1);
    ActivateTab(g_active);
}

static void ReopenClosedTab() {
    if (g_closedTabs.empty()) return;
    SessionTab st = g_closedTabs.back();
    g_closedTabs.pop_back();
    AddTab(st.left, true, g_active + 1, &st.right, st.activePane);
}

static std::wstring PaneTitle(const Pane& t);

static std::wstring PaneTitle(const Pane& t) {
    if (!t.tagFilter.empty()) {
        if (const TagDef* d = TagDefById(t.tagFilter)) return L"Tag: " + d->name;
    }
    if (t.path.empty()) return L"This PC";
    if (t.path.size() == 3 && t.path[1] == L':') {
        auto it = g_driveNames.find(t.path);
        if (it != g_driveNames.end()) return it->second;
    }
    return PathLeaf(t.path);
}

// ============================================================ opening + commands
static void OpenFolderInNewTab(const std::wstring& path, bool activate) { AddTab(path, activate, g_active + 1); }

static void OpenView(Pane& t, int v, bool newTab) {
    if (v < 0 || v >= (int)t.view.size()) return;
    const Item& it = t.items[t.view[v]];
    std::wstring path = ItemPath(t, it);
    if (it.isDir()) {
        if (newTab) OpenFolderInNewTab(path, false);
        else Navigate(t, path);
        return;
    }
    // archives open like folders (Strata falls back to the default app if Windows can't read that format)
    if (IsArchiveFile(path) && GetKeyState(VK_SHIFT) >= 0) {
        if (newTab) OpenFolderInNewTab(path, false);
        else Navigate(t, path);
        return;
    }
    // a file inside an archive: extract it to a temp folder first
    if (!ArchiveOf(t.path).empty()) {
        std::wstring tmp;
        if (ExtractToTemp(g_hwnd, path, tmp)) OpenItem(g_hwnd, tmp, nullptr);
        else SetStatus(L"Couldn't extract " + it.name);
        return;
    }
    std::wstring target;
    if (!OpenItem(g_hwnd, path, &target)) {
        if (newTab) OpenFolderInNewTab(target, false);
        else Navigate(t, target);
    }
}

static void OpenSelection(Pane& t, bool newTab) {
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

// ---- undo (Ctrl+Z)
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct UndoRec {
    enum Kind { Rename, Move, Copy, Create, Recycle } kind;
    std::vector<std::pair<std::wstring, std::wstring>> pairs;   // Rename / Move: (from, to)
    std::vector<std::wstring> paths;                             // Copy / Create: created items; Recycle: original paths
};
}  // namespace
static std::vector<UndoRec> g_undo;
static bool g_undoSkipNextRecycle = false;   // the Recycle Bin delete that an undo performs is not itself undoable

static void PushUndo(UndoRec r) {
    g_undo.push_back(std::move(r));
    if (g_undo.size() > 50) g_undo.erase(g_undo.begin());
}

static void ReloadPanesFor(const std::vector<std::wstring>& paths) {
    // reload any visible pane showing one of these folders (or their parents)
    std::set<std::wstring> folders;
    for (auto& p : paths) { folders.insert(ToLower(p)); folders.insert(ToLower(PathParent(p))); }
    for (auto& tb_ : g_tabs) for (auto& pn : tb_.pane) {
        if (pn.searchActive || !pn.tagFilter.empty()) continue;
        if (folders.count(ToLower(pn.path)) && pn.loaded) Load(pn, true);
    }
}

static GitResult g_folderGit;                    // Git status of the active pane's folder
static bool g_installWhenReady = false;
static bool g_queueOpen = false;
static DWORD g_queueOpenedAt = 0;
static bool g_queueTouched = false;

static void StartTransfer(JobKind kind, const std::vector<std::wstring>& files, const std::wstring& dest) {
    if (files.empty() || dest.empty()) return;
    if (!ArchiveOf(dest).empty()) { SetStatus(L"Archives are read-only in Strata — extract first, then add files with 7-Zip or Explorer"); return; }
    // items inside an archive are copied out by the Windows shell
    bool fromArchive = false;
    for (auto& f : files) { std::wstring a = ArchiveOf(f); if (!a.empty() && _wcsicmp(a.c_str(), f.c_str()) != 0) fromArchive = true; }
    if (fromArchive) {
        if (kind == JobKind::Move) { SetStatus(L"Items can't be moved out of an archive — copying instead"); }
        ShellCopyOut(g_hwnd, files, dest);
        return;
    }
    QueueAdd(kind, files, dest, g_config.verifyCopies);
    g_queueOpen = true;
    g_queueOpenedAt = GetTickCount();
    g_queueTouched = false;
    Redraw();
}

static void DoCopyCut(bool cut) {
    Pane& t = T();
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
    StartTransfer(cut ? JobKind::Move : JobKind::Copy, files, destFolder);
    if (cut) { OpenClipboard(g_hwnd); EmptyClipboard(); CloseClipboard(); }
}

// F5 / F6: copy or move the selection to the folder in the other pane
static void TransferToOtherPane(JobKind kind) {
    Pane& t = T();
    Pane& o = OtherPane();
    if (DriveView(t)) return;
    if (!DualMode()) { SetStatus(L"F5 / F6 copy and move between panes — switch to dual-pane view first"); return; }
    if (o.path.empty() || o.searchActive || !o.tagFilter.empty()) { SetStatus(L"Open a folder in the other pane first"); return; }
    auto paths = SelectedPaths(t);
    if (paths.empty() && t.focus >= 0 && t.focus < (int)t.view.size()) paths.push_back(ItemPath(t, t.items[t.view[t.focus]]));
    if (paths.empty()) return;
    StartTransfer(kind, paths, o.path);
}

static void DoDelete(bool permanent) {
    Pane& t = T();
    if (DriveView(t) || !t.archive.empty()) return;
    auto paths = SelectedPaths(t);
    if (paths.empty()) return;
    if (permanent || g_config.confirmDelete) {
        std::wstring what = paths.size() == 1 ? L"\"" + PathLeaf(paths[0]) + L"\"" : std::to_wstring(paths.size()) + L" items";
        std::wstring q = permanent ? L"Permanently delete " + what + L"?\n\nThis can't be undone." : L"Move " + what + L" to the Recycle Bin?";
        if (MessageBoxW(g_hwnd, q.c_str(), L"Strata", MB_OKCANCEL | (permanent ? MB_ICONWARNING : MB_ICONQUESTION) | MB_DEFBUTTON2) != IDOK) return;
    }
    FileOpAsync(g_hwnd, permanent ? OpKind::Delete : OpKind::Recycle, paths, L"");
}

static void Undo() {
    if (g_undo.empty()) { SetStatus(L"Nothing to undo"); return; }
    UndoRec r = std::move(g_undo.back());
    g_undo.pop_back();
    std::vector<std::wstring> touched;
    switch (r.kind) {
        case UndoRec::Rename: {
            if (r.pairs.size() > 1) {
                std::vector<std::pair<std::wstring, std::wstring>> back;
                for (auto& p : r.pairs) back.push_back({p.second, p.first});
                std::wstring err;
                auto done = BatchRenameApply(back, &err);
                for (auto& d : done) { TagsPathMoved(d.first, d.second); touched.push_back(d.second); }
                SetStatus(L"Undid rename of " + std::to_wstring(done.size()) + L" items");
                break;
            }
            for (auto& p : r.pairs) {
                std::wstring err;
                if (RenameItem(g_hwnd, p.second, PathLeaf(p.first), &err)) { TagsPathMoved(p.second, p.first); touched.push_back(p.first); }
                else { SetStatus(L"Couldn't undo the rename" + (err.empty() ? L"" : L": " + err)); return; }
            }
            SetStatus(L"Undid rename");
            break;
        }
        case UndoRec::Move: {
            std::vector<std::pair<std::wstring, std::wstring>> back;
            for (auto& p : r.pairs) back.push_back({p.second, p.first});
            QueueAddPairs(JobKind::Move, back, L"Undoing move of " + std::to_wstring(back.size()) + (back.size() == 1 ? L" item" : L" items"));
            SetStatus(L"Undoing move…");
            break;
        }
        case UndoRec::Copy:
        case UndoRec::Create:
            FileOpAsync(g_hwnd, OpKind::Recycle, r.paths, L"");
            SetStatus(r.kind == UndoRec::Copy ? L"Undid copy (the copies went to the Recycle Bin)" : L"Undid new item");
            g_undoSkipNextRecycle = true;
            break;
        case UndoRec::Recycle: {
            size_t n = RestoreFromRecycleBin(g_hwnd, r.paths);
            SetStatus(n ? L"Restored " + std::to_wstring(n) + (n == 1 ? L" item" : L" items") + L" from the Recycle Bin"
                        : L"Couldn't find those items in the Recycle Bin");
            touched = r.paths;
            break;
        }
    }
    ReloadPanesFor(touched);
}

static void SetFocusTo(Focus f);

static void BatchRename() {
    Pane& t = T();
    auto paths = SelectedPaths(t);
    auto plan = BatchRenameDialog(g_hwnd, paths);
    if (plan.empty()) return;
    std::wstring err;
    auto done = BatchRenameApply(plan, &err);
    if (!done.empty()) {
        PushUndo({UndoRec::Rename, done, {}});
        for (auto& d : done) TagsPathMoved(d.first, d.second);
        t.restoreSel.clear();
        for (auto& d : done) t.restoreSel.insert(PathLeaf(d.second));
    }
    SetStatus(!err.empty() ? err : L"Renamed " + std::to_wstring(done.size()) + L" items   ·   Ctrl+Z to undo");
    if (t.searchActive || !t.tagFilter.empty()) {
        for (auto& it : t.items)
            for (auto& d : done) if (_wcsicmp(it.path.c_str(), d.first.c_str()) == 0) { it.path = d.second; it.name = PathLeaf(d.second); }
        BuildView(t);
    } else {
        Load(t, true);
    }
}

static void BeginRename() {
    Pane& t = T();
    if (DriveView(t) || !t.archive.empty() || t.focus < 0 || t.focus >= (int)t.view.size()) return;
    if (SelectedCount(t) > 1) { BatchRename(); return; }
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
    Pane& t = T();
    if (commit && g_renameItem >= 0 && g_renameItem < (int)t.items.size()) {
        const Item& it = t.items[g_renameItem];
        std::wstring newName = g_renameInput.text;
        while (!newName.empty() && (newName.back() == L' ' || newName.back() == L'.')) newName.pop_back();
        while (!newName.empty() && newName.front() == L' ') newName.erase(newName.begin());
        if (!newName.empty() && newName != it.name) {
            std::wstring err;
            std::wstring oldPath = ItemPath(t, it);
            std::wstring newPath = PathJoin(PathParent(oldPath), newName);
            if (RenameItem(g_hwnd, oldPath, newName, &err)) {
                PushUndo({UndoRec::Rename, {{oldPath, newPath}}, {}});
                TagsPathMoved(oldPath, newPath);
                t.items[g_renameItem].name = newName;
                if (!t.items[g_renameItem].path.empty()) t.items[g_renameItem].path = newPath;
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
    Pane& t = T();
    if (t.path.empty() || t.searchActive || !t.tagFilter.empty() || !t.archive.empty()) return;
    std::wstring name = UniqueName(t.path, L"New folder", L"");
    if (!CreateDirectoryW(PathJoin(t.path, name).c_str(), nullptr)) {
        SetStatus(L"Couldn't create a folder here: " + ErrorText(GetLastError()));
        return;
    }
    PushUndo({UndoRec::Create, {}, {PathJoin(t.path, name)}});
    t.selectAfterLoad = name;
    t.renameAfterLoad = true;
    Load(t, false);
}

static void NewTextFile() {
    Pane& t = T();
    if (t.path.empty() || t.searchActive || !t.tagFilter.empty() || !t.archive.empty()) return;
    std::wstring name = UniqueName(t.path, L"New text document", L".txt");
    HANDLE h = CreateFileW(PathJoin(t.path, name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SetStatus(L"Couldn't create a file here: " + ErrorText(GetLastError())); return; }
    CloseHandle(h);
    PushUndo({UndoRec::Create, {}, {PathJoin(t.path, name)}});
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
    Pane& t = T();
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
static void AddZone(ZoneKind k, int idx, const D2D1_RECT_F& r, int pane = -1) {
    g_zones.push_back({k, idx, r, pane});
}
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
static std::wstring UpdatePillText() {
    updater::Status st = updater::Get();
    switch (st.state) {
        case updater::State::Available: return L"Update to " + st.release.version;
        case updater::State::Downloading: return L"Downloading " + std::to_wstring(st.percent) + L"%";
        case updater::State::Verifying: return L"Verifying…";
        case updater::State::Ready: return L"Restart to update";
        case updater::State::Installing: return L"Updating…";
        default: return L"";
    }
}
static float UpdatePillW() {
    std::wstring t = UpdatePillText();
    return t.empty() ? 0.0f : MeasureWidth(t, g_fCenter) + 32;
}

static float TabsLeft() { return 12 + 18 + 10 + MeasureWidth(L"Strata", g_fTitle) + 14; }

static void TabGeometry(float& left, float& tabW) {
    left = TabsLeft();
    float right = g_W - 3 * kCapW - 44 - UpdatePillW();
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
        const Pane& t = g_tabs[i].pane[g_tabs[i].active];
        float cy = (r.top + r.bottom) / 2 - (act ? 0 : 1);
        bool showClose = act || Hot(ZTab, i) || Hot(ZTabClose, i);
        float textRight = r.right - (showClose ? 30 : 10);
        if (t.path.empty()) g_cv.Glyph(0xE7F8, g_fIcon, R(r.left + 8, cy - 8, r.left + 26, cy + 8), g_theme.accent);
        else DrawItemIcon(folderIcon, r.left + 10, cy, 16);
        g_cv.Text(PaneTitle(t), act ? g_fUiSemi : g_fUi, R(r.left + 32, r.top, textRight, r.bottom - (act ? 0 : 2)), act ? g_theme.text : textC);
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
        const Pane& t = g_tabs[g_dragIdx].pane[g_tabs[g_dragIdx].active];
        float cy = (r.top + r.bottom) / 2;
        if (t.path.empty()) g_cv.Glyph(0xE7F8, g_fIcon, R(r.left + 8, cy - 8, r.left + 26, cy + 8), g_theme.accent);
        else DrawItemIcon(folderIcon, r.left + 10, cy, 16);
        g_cv.Text(PaneTitle(t), g_fUiSemi, R(r.left + 32, r.top, r.right - 10, r.bottom), g_theme.text);
    }
    // new tab
    float nx = left + g_tabs.size() * tabW + 4;
    nx = std::min(nx, g_W - 3 * kCapW - 40 - UpdatePillW());
    DrawIconButton(ZNewTab, R(nx, 8, nx + 30, kTitleH - 4), 0xE710, true);

    // "update available" pill, left of the window buttons
    if (UpdatePillW() > 0) {
        std::wstring label = UpdatePillText();
        float w = UpdatePillW() - 8;
        D2D1_RECT_F r = R(g_W - 3 * kCapW - w - 8, 8, g_W - 3 * kCapW - 8, kTitleH - 8);
        AddZone(ZUpdatePill, -1, r);
        g_cv.FillRound(r, (r.bottom - r.top) / 2, Hot(ZUpdatePill) ? g_theme.accent : Alpha(g_theme.accent, 0.85f));
        g_cv.Text(label, g_fCenter, r, g_theme.onAccent);
    }

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
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Crumb { std::wstring label, path; D2D1_RECT_F rc, arrow; };
}  // namespace
static std::vector<Crumb> g_crumbs;
static std::vector<std::wstring> g_hiddenCrumbs;   // ancestors elided from the bar

static void BuildCrumbs(float boxLeft, float boxRight) {
    g_crumbs.clear();
    g_hiddenCrumbs.clear();
    const Pane& t = T();
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
    Pane& t = T();
    float cy = (top + bottom) / 2, bh = 32;
    float x = 10;
    if (g_config.showNavButtons) {
        DrawIconButton(ZBack, R(x, cy - bh / 2, x + 34, cy + bh / 2), 0xE72B, !t.back.empty()); x += 36;
        DrawIconButton(ZFwd, R(x, cy - bh / 2, x + 34, cy + bh / 2), 0xE72A, !t.fwd.empty()); x += 36;
        DrawIconButton(ZUp, R(x, cy - bh / 2, x + 34, cy + bh / 2), 0xE74A, !t.path.empty()); x += 36;
        DrawIconButton(ZRefresh, R(x, cy - bh / 2, x + 34, cy + bh / 2), 0xE72C, true); x += 42;
    }

    float menuW = 34;
    float searchW = std::clamp(g_W * 0.22f, 170.0f, 320.0f);
    float right = g_W - 10;
    D2D1_RECT_F menu = R(right - menuW, cy - bh / 2, right, cy + bh / 2);
    DrawIconButton(ZMenu, menu, 0xE712, true);
    right -= menuW + 4;
    // preview panel toggle
    {
        D2D1_RECT_F pr = R(right - 34, cy - bh / 2, right, cy + bh / 2);
        AddZone(ZPreviewToggle, -1, pr);
        if (g_config.showPreview) g_cv.FillRound(pr, 6, Alpha(g_theme.accent, 0.18f));
        else if (Hot(ZPreviewToggle)) g_cv.FillRound(pr, 6, g_theme.hover);
        // drawn icon: a page with a panel on the right
        float cx = (pr.left + pr.right) / 2;
        D2D1_RECT_F ic = R(cx - 8, cy - 6.5f, cx + 8, cy + 6.5f);
        D2D1_COLOR_F c = g_config.showPreview ? g_theme.accent : g_theme.text;
        g_cv.StrokeRound(ic, 2, c, 1.2f);
        g_cv.Fill(R(cx + 2, cy - 6, cx + 3.2f, cy + 6), c);
        right -= 34 + 6;
    }
    // view switch: dual pane / single pane / grid
    if (g_config.showViewSwitch) {
        float w = 30;
        D2D1_RECT_F seg = R(right - 3 * w - 4, cy - 15, right, cy + 15);
        g_cv.FillRound(seg, 7, g_theme.surface);
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F r = R(seg.left + 2 + i * w, seg.top + 2, seg.left + 2 + (i + 1) * w, seg.bottom - 2);
            AddZone(ZViewSwitch, i, r);
            bool on = (i == 0 && DualMode() && t.viewMode == ViewList) || (i == 1 && !DualMode() && t.viewMode == ViewList) || (i == 2 && t.viewMode == ViewGrid);
            if (on) g_cv.FillRound(r, 5, Alpha(g_theme.accent, 0.22f));
            else if (Hot(ZViewSwitch, i)) g_cv.FillRound(r, 5, g_theme.hover);
            D2D1_COLOR_F c = on ? g_theme.accent : g_theme.subtext;
            float cx = (r.left + r.right) / 2, ccy = (r.top + r.bottom) / 2;
            if (i == 0) {
                g_cv.StrokeRound(R(cx - 8, ccy - 6, cx - 1, ccy + 6), 1.5f, c, 1.2f);
                g_cv.StrokeRound(R(cx + 1, ccy - 6, cx + 8, ccy + 6), 1.5f, c, 1.2f);
            } else if (i == 1) {
                g_cv.StrokeRound(R(cx - 8, ccy - 6, cx + 8, ccy + 6), 1.5f, c, 1.2f);
                for (int k = 0; k < 3; ++k) g_cv.Fill(R(cx - 5, ccy - 3 + k * 3, cx + 5, ccy - 2 + k * 3), c);
            } else {
                for (int a = 0; a < 2; ++a)
                    for (int b = 0; b < 2; ++b) g_cv.StrokeRound(R(cx - 7 + a * 8, ccy - 7 + b * 8, cx - 1 + a * 8, ccy - 1 + b * 8), 1.2f, c, 1.2f);
            }
        }
        right = seg.left - 8;
    }
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
        // Git badge for repositories
        if (g_folderGit.isRepo && _wcsicmp(g_folderGit.folder.c_str(), t.path.c_str()) == 0 && !g_crumbs.empty()) {
            std::wstring txt = g_folderGit.branch;
            if (g_folderGit.changed > 0 || g_folderGit.untracked > 0) txt += L"  \x2022 " + std::to_wstring(g_folderGit.changed + g_folderGit.untracked);
            float tw = MeasureWidth(txt, g_fSmall) + 34;
            float lastRight = g_crumbs.back().arrow.right;
            if (g_rcPathBox.right - tw - 6 > lastRight) {
                D2D1_RECT_F b = R(g_rcPathBox.right - tw - 6, g_rcPathBox.top + 6, g_rcPathBox.right - 6, g_rcPathBox.bottom - 6);
                bool dirty = g_folderGit.changed > 0 || g_folderGit.untracked > 0;
                D2D1_COLOR_F c = dirty ? Rgb(0xF5A524) : Rgb(0x30A46C);
                g_cv.FillRound(b, (b.bottom - b.top) / 2, Alpha(c, 0.16f));
                g_cv.Glyph(0xE8D0, g_fIconSmall, R(b.left + 6, b.top, b.left + 22, b.bottom), c);
                g_cv.Text(txt, g_fSmall, R(b.left + 24, b.top, b.right - 6, b.bottom), g_theme.text);
            }
        }
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
        g_cv.Text(t.path.empty() ? std::wstring(L"Search all drives") : L"Search " + PaneTitle(t), g_fUi, stb, Alpha(g_theme.subtext, 0.8f));
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
static bool g_sideCollapsed[6] = {false, false, false, true, false, false};   // pinned, drives, tags, recent, saved, -
static void ToggleSidebarSection(int i) { if (i >= 0 && i < 6) g_sideCollapsed[i] = !g_sideCollapsed[i]; }

static void PaintSidebar() {
    float top = kTitleH + kToolH, bottom = g_H - (g_config.showStatusBar ? kStatusH : 0);
    float w = g_config.sidebarWidth;
    g_rcSidebar = R(0, top, w, bottom);
    g_cv.Fill(g_rcSidebar, SidebarColor());
    AddZone(ZSideSplitter, -1, R(w - kSplitW / 2, top, w + kSplitW / 2, bottom));
    if (g_drag == Drag::Splitter || Hot(ZSideSplitter)) g_cv.Fill(R(w - 1, top, w + 1, bottom), Alpha(g_theme.accent, 0.6f));
    else g_cv.Fill(R(w - 0.5f, top, w + 0.5f, bottom), g_theme.divider);

    g_rt->PushAxisAlignedClip(R(0, top, w - 1, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = top + 8 - g_sideScroll;
    const Pane& t = T();
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
    auto section = [&](int idx, const wchar_t* label) {
        D2D1_RECT_F r = R(8, y, w - 8, y + 26);
        AddZone(ZSideSection, idx, r);
        g_cv.Text(label, g_fSection, R(20, y, w - 30, y + 26), Hot(ZSideSection, idx) ? g_theme.text : g_theme.subtext);
        g_cv.Glyph(g_sideCollapsed[idx] ? 0xE76C : 0xE70D, g_fIconSmall, R(w - 30, y, w - 12, y + 26), Alpha(g_theme.subtext, Hot(ZSideSection, idx) ? 1.0f : 0.5f));
        y += 26;
        return !g_sideCollapsed[idx];
    };
    int folderIcon = FolderIconIndex();
    // Pinned
    if (section(0, L"PINNED")) {
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
    }
    // Drives
    y += 6;
    if (section(1, L"DRIVES"))
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
    // Tags
    if (g_config.sidebarTags && !g_config.tags.empty()) {
        y += 6;
        if (section(2, L"TAGS"))
            for (int i = 0; i < (int)g_config.tags.size(); ++i) {
                const TagDef& tg = g_config.tags[i];
                D2D1_RECT_F r = R(8, y, w - 8, y + 30);
                AddZone(ZSideTag, i, r);
                rowBg(r, t.tagFilter == tg.id, Hot(ZSideTag, i));
                g_cv.Circle(r.left + 18, (r.top + r.bottom) / 2, 5, Rgb(tg.color));
                g_cv.Text(tg.name, g_fUi, R(r.left + 38, r.top, r.right - 8, r.bottom), g_theme.text);
                y += 32;
            }
    }
    // Saved searches (smart folders)
    if (g_config.sidebarSaved && !g_config.savedSearches.empty()) {
        y += 6;
        if (section(4, L"SAVED SEARCHES"))
            for (int i = 0; i < (int)g_config.savedSearches.size(); ++i) {
                const SavedSearch& ss = g_config.savedSearches[i];
                D2D1_RECT_F r = R(8, y, w - 8, y + 30);
                AddZone(ZSideSaved, i, r);
                bool cur = t.searchActive && t.searchQuery == ss.query && _wcsicmp(t.path.c_str(), ss.root.c_str()) == 0;
                rowBg(r, cur, Hot(ZSideSaved, i));
                g_cv.Glyph(0xE721, g_fIcon, R(r.left + 8, r.top, r.left + 30, r.bottom), g_theme.accent);
                g_cv.Text(ss.name, g_fUi, R(r.left + 38, r.top, r.right - 8, r.bottom), g_theme.text);
                y += 32;
            }
    }
    // Recent folders
    if (g_config.sidebarRecent && !g_config.recentFolders.empty()) {
        y += 6;
        if (section(3, L"RECENT"))
            for (int i = 0; i < (int)g_config.recentFolders.size() && i < 10; ++i) {
                const std::wstring& rp = g_config.recentFolders[i];
                D2D1_RECT_F r = R(8, y, w - 8, y + 30);
                AddZone(ZSideRecent, i, r);
                rowBg(r, false, Hot(ZSideRecent, i));
                DrawItemIcon(folderIcon, r.left + 10, (r.top + r.bottom) / 2, 16, 0.85f);
                g_cv.Text(PathLeaf(rp), g_fUi, R(r.left + 38, r.top, r.right - 8, r.bottom), g_theme.text);
                y += 32;
            }
    }
    g_sideContentH = y + g_sideScroll - top + 8;
    g_rt->PopAxisAlignedClip();
}

// ============================================================ file list
static const wchar_t* kColLabel[ColCount] = {L"Name", L"Size", L"Modified", L"Type"};

static void ComputeColumns(Pane& t, float left) {
    t.cols.clear();
    bool thisPC = DriveView(t);
    float avail = std::max(200.0f, t.rcList.right - left - 18);
    float w[ColCount];
    bool vis[ColCount];
    for (int c = 0; c < ColCount; ++c) {
        vis[c] = g_config.columnVisible[c] || c == ColName;
        w[c] = g_config.columnWidth[c];
        if (thisPC && c == ColModified) w[c] = std::max(w[c], 200.0f);
        if ((t.searchActive || !t.tagFilter.empty()) && c == ColType) w[c] = std::max(w[c], 260.0f);   // Location
    }
    // Fit narrow panes (dual-pane mode): shrink Name first, then drop Type, then Modified
    auto total = [&] { float s = 0; for (int c = 0; c < ColCount; ++c) if (vis[c]) s += w[c]; return s; };
    if (total() > avail) w[ColName] = std::max(150.0f, w[ColName] - (total() - avail));
    if (total() > avail && vis[ColType]) vis[ColType] = false;
    if (total() > avail) w[ColName] = std::max(150.0f, w[ColName] - (total() - avail));
    if (total() > avail && vis[ColModified]) w[ColModified] = std::max(110.0f, w[ColModified] - (total() - avail));
    if (total() > avail && vis[ColModified]) vis[ColModified] = false;
    if (total() > avail) w[ColName] = std::max(120.0f, w[ColName] - (total() - avail));
    // Wide panes: give the spare room to Name
    if (total() < avail && w[ColName] < g_config.columnWidth[ColName]) w[ColName] = std::min(g_config.columnWidth[ColName], w[ColName] + (avail - total()));
    float x = left;
    for (int c = 0; c < ColCount; ++c) {
        if (!vis[c]) continue;
        t.cols.push_back({c, x, x + w[c]});
        x += w[c];
    }
}

// Item (view index) under a point in a pane, or -1. x < 0 ignores the horizontal position (list view).
static int ItemAt(const Pane& t, float x, float y) {
    if (y < t.rcList.top || y >= t.rcList.bottom) return -1;
    if (t.viewMode == ViewGrid) {
        if (x < 0) return -1;
        int col = (int)std::floor((x - t.rcList.left - 8) / t.tileW);
        if (col < 0 || col >= t.gridCols) return -1;
        int row = (int)std::floor((y - t.rcList.top - 4 + t.scroll) / t.tileH);
        int v = row * t.gridCols + col;
        return v >= 0 && v < (int)t.view.size() ? v : -1;
    }
    if (x >= 0 && x > t.rowRight) return -1;
    int v = (int)std::floor((y - t.rcList.top + t.scroll) / RowH());
    return v >= 0 && v < (int)t.view.size() ? v : -1;
}
static int RowAt(float y, float x = -1) { return ItemAt(T(), x, y); }

static D2D1_RECT_F ScrollTrack(const Pane& t) { return R(t.rcList.right - 12, t.rcList.top + 2, t.rcList.right - 2, t.rcList.bottom - 2); }
static D2D1_RECT_F ScrollTrack() { return ScrollTrack(T()); }

static bool ScrollThumb(const Pane& t, D2D1_RECT_F& thumb) {
    float content = ContentH(t), h = ListViewH(t);
    if (content <= h + 1) return false;
    D2D1_RECT_F tr = ScrollTrack(t);
    float th = std::max(32.0f, (tr.bottom - tr.top) * h / content);
    float maxS = MaxScroll(t);
    float pos = maxS > 0 ? t.scroll / maxS : 0;
    float y = tr.top + (tr.bottom - tr.top - th) * pos;
    thumb = R(tr.left, y, tr.right, y + th);
    return true;
}
static bool ScrollThumb(D2D1_RECT_F& thumb) { return ScrollThumb(T(), thumb); }

static int g_hotPane = -1;
static std::wstring g_dropFolder;                 // drag-and-drop target being hovered
static bool HotP(ZoneKind k, int idx, int pane) { return g_hotKind == k && g_hotIdx == idx && g_hotPane == pane; }

static std::wstring PanePathOf(const Pane& t, const Item& it) { return ItemPath(t, it); }

static uint64_t DirSize(const Pane& t, const Item& it, bool& known) {
    FolderSize fs;
    known = SizesGet(PanePathOf(t, it), fs) && fs.done;
    return known ? fs.bytes : 0;
}

static void EmptyState(Pane& t) {
    float cx = (t.rcList.left + t.rcList.right) / 2, cy = t.rcList.top + std::min(160.0f, ListViewH(t) * 0.35f);
    std::wstring title, sub;
    wchar_t glyph = 0xE8B7;
    if (t.searchActive && t.searchRunning) {
        glyph = 0xE721;
        title = L"Searching…";
        sub = t.path.empty() ? L"Looking through every drive" : L"Looking through " + PaneTitle(t) + L" and its subfolders";
        if (t.searchScanned) sub += L" (" + WithCommas(t.searchScanned) + L" folders so far)";
    } else if (t.searchActive) {
        glyph = 0xE721;
        title = L"No results";
        sub = L"Nothing found for \"" + t.searchQuery + L"\"";
    } else if (!t.tagFilter.empty()) {
        glyph = 0xE8EC;
        title = L"Nothing tagged yet";
        sub = L"Right-click a file or folder → Tag";
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
        sub = L"Paste or drop files here, or right-click for New";
    }
    if (title.empty()) return;
    if (!(t.loading && !t.loaded)) g_cv.Glyph(glyph, g_fIconBig, R(cx - 40, cy - 30, cx + 40, cy + 30), Alpha(g_theme.subtext, 0.55f));
    g_cv.Text(title, g_fEmpty, R(t.rcList.left + 20, cy + 34, t.rcList.right - 20, cy + 60), g_theme.text);
    if (!sub.empty()) g_cv.Text(sub, g_fCenter, R(t.rcList.left + 20, cy + 60, t.rcList.right - 20, cy + 82), g_theme.subtext);
}

static void DrawTagDot(const std::wstring& path, float x, float cy) {
    uint32_t c = TagColor(TagOf(path));
    if (c) g_cv.Circle(x, cy, 3.5f, Rgb(c));
}

static ID2D1Bitmap* ThumbFor(const std::wstring& path, int px);   // grid thumbnails (thumbs section)

static void PaintGrid(Pane& t, int paneIdx, bool active) {
    float avail = t.rcList.right - t.rcList.left - 16 - 10;
    float base = g_config.iconSize == L"small" ? 104 : g_config.iconSize == L"large" ? 176 : 136;
    t.gridCols = std::max(1, (int)(avail / base));
    t.tileW = avail / t.gridCols;
    t.tileH = t.tileW + 30;
    ClampScroll(t);
    bool focusList = active && (g_focus == Focus::List || g_focus == Focus::Rename);
    int firstRow = std::max(0, (int)std::floor((t.scroll - 4) / t.tileH));
    int lastRow = (int)std::ceil((t.scroll + ListViewH(t)) / t.tileH);
    g_rcRenameBox = active ? R(0, 0, 0, 0) : g_rcRenameBox;
    for (int row = firstRow; row <= lastRow; ++row) {
        for (int c = 0; c < t.gridCols; ++c) {
            int v = row * t.gridCols + c;
            if (v >= (int)t.view.size()) break;
            int idx = t.view[v];
            const Item& it = t.items[idx];
            float x = t.rcList.left + 8 + c * t.tileW, y = t.rcList.top + 4 + row * t.tileH - t.scroll;
            D2D1_RECT_F tile = R(x + 3, y + 3, x + t.tileW - 3, y + t.tileH - 3);
            bool sel = t.sel[idx] != 0;
            std::wstring full = ItemPath(t, it);
            if (sel) g_cv.FillRound(tile, 8, Alpha(g_theme.accent, focusList ? 0.24f : 0.13f));
            else if (g_hotRow == v && g_hotPane == paneIdx && g_drag == Drag::None) g_cv.FillRound(tile, 8, g_theme.hover);
            if (!g_dropFolder.empty() && it.isDir() && _wcsicmp(full.c_str(), g_dropFolder.c_str()) == 0)
                g_cv.StrokeRound(tile, 8, g_theme.accent, 2);
            else if (v == t.focus && focusList && !sel) g_cv.StrokeRound(tile, 8, Alpha(g_theme.accent, 0.45f), 1);
            float imgSz = t.tileW - 30;
            D2D1_RECT_F img = R(x + (t.tileW - imgSz) / 2, y + 10, x + (t.tileW + imgSz) / 2, y + 10 + imgSz);
            ID2D1Bitmap* bmp = DriveView(t) ? nullptr : ThumbFor(full, (int)(imgSz * g_scale));
            if (bmp) {
                D2D1_SIZE_F bs = bmp->GetSize();
                float k = std::min((img.right - img.left) / std::max(1.0f, bs.width), (img.bottom - img.top) / std::max(1.0f, bs.height));
                k = std::min(k, 1.0f * g_scale);   // don't upscale small thumbnails too much
                float w = bs.width * k, h = bs.height * k;
                float cx = (img.left + img.right) / 2, cy = (img.top + img.bottom) / 2;
                g_rt->DrawBitmap(bmp, R(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                float isz = std::min(48.0f, imgSz * 0.5f);
                DrawItemIcon(it.icon, (img.left + img.right) / 2 - isz / 2, (img.top + img.bottom) / 2, isz);
            }
            DrawTagDot(full, tile.left + 10, tile.top + 10);
            D2D1_RECT_F nr = R(x + 6, img.bottom + 2, x + t.tileW - 6, y + t.tileH - 4);
            if (active && g_focus == Focus::Rename && idx == g_renameItem) {
                g_rcRenameBox = R(nr.left, nr.top + 2, nr.right, nr.bottom - 2);
                g_cv.FillRound(g_rcRenameBox, 4, g_theme.surfaceStrong);
                g_cv.StrokeRound(g_rcRenameBox, 4, g_theme.accent, 1.2f);
                g_renameInput.Draw(g_cv, g_fInput, R(g_rcRenameBox.left + 4, g_rcRenameBox.top, g_rcRenameBox.right - 4, g_rcRenameBox.bottom),
                                   g_theme.text, Alpha(g_theme.accent, 0.4f), g_theme.text, true);
            } else {
                g_cv.Text(DisplayName(it), g_fCenter, nr, (it.attrs & FILE_ATTRIBUTE_HIDDEN) ? Alpha(g_theme.text, 0.55f) : g_theme.text);
            }
        }
    }
}

static void PaintPane(Pane& t, int paneIdx, const D2D1_RECT_F& area, bool active) {
    float left = area.left, top = area.top, bottom = area.bottom, right = area.right;
    t.rcPane = area;
    g_cv.Fill(area, MainColor());
    t.rcHeader = R(left, top, right, top + kHeaderH);
    t.rcList = R(left, top + kHeaderH, right, bottom);
    ClampScroll(t);
    ComputeColumns(t, left + 12);
    bool thisPC = DriveView(t);
    bool dual = DualMode();
    bool listFocus = active && (g_focus == Focus::List || g_focus == Focus::Rename);
    bool locationCol = t.searchActive || !t.tagFilter.empty();

    // header (in grid view it shows the sort choice; the columns themselves are hidden)
    g_rt->PushAxisAlignedClip(t.rcHeader, D2D1_ANTIALIAS_MODE_ALIASED);
    if (dual && active) g_cv.Fill(R(left, top, right, top + 2), g_theme.accent);
    if (t.viewMode == ViewGrid) {
        float x = left + 12;
        for (int c = 0; c < ColCount; ++c) {
            const wchar_t* label = kColLabel[c];
            if (c == ColType && locationCol) label = L"Location";
            float w = MeasureWidth(label, g_fHeader) + 30;
            D2D1_RECT_F r = R(x, top + 4, x + w, top + kHeaderH - 4);
            AddZone(ZHeader, c, r, paneIdx);
            bool sorted = t.sortColumn == c;
            if (sorted) g_cv.FillRound(r, 5, Alpha(g_theme.accent, 0.14f));
            else if (HotP(ZHeader, c, paneIdx)) g_cv.FillRound(r, 5, g_theme.hover);
            g_cv.Text(label, g_fHeader, R(r.left + 10, r.top, r.right - 16, r.bottom), sorted ? g_theme.text : g_theme.subtext);
            if (sorted) g_cv.Glyph(t.sortDescending ? 0xE96E : 0xE96D, g_fIconSmall, R(r.right - 18, r.top, r.right - 4, r.bottom), g_theme.accent);
            x += w + 4;
        }
    } else {
        for (auto& c : t.cols) {
            D2D1_RECT_F r = R(c.left, top, c.right, top + kHeaderH);
            AddZone(ZHeader, c.col, r, paneIdx);
            if (HotP(ZHeader, c.col, paneIdx)) g_cv.FillRound(R(r.left, r.top + 4, r.right, r.bottom - 4), 5, g_theme.hover);
            bool sorted = t.sortColumn == c.col;
            bool rightAlign = c.col == ColSize;
            const wchar_t* label = kColLabel[c.col];
            if (thisPC && c.col == ColModified) label = L"Space";
            if (locationCol && c.col == ColType) label = L"Location";
            float tw = MeasureWidth(label, g_fHeader);
            D2D1_RECT_F tr = rightAlign ? R(r.right - 12 - tw, r.top, r.right - 12, r.bottom) : R(r.left + 10, r.top, r.right - 18, r.bottom);
            g_cv.Text(label, g_fHeader, tr, sorted ? g_theme.text : g_theme.subtext);
            if (sorted) {
                float ax = rightAlign ? tr.left - 16 : std::min(tr.left + tw + 4, r.right - 16);
                g_cv.Glyph(t.sortDescending ? 0xE96E : 0xE96D, g_fIconSmall, R(ax, r.top, ax + 14, r.bottom), g_theme.accent);
            }
            D2D1_RECT_F div = R(c.right - 4, top + 6, c.right + 4, top + kHeaderH - 6);
            AddZone(ZHeaderDiv, c.col, div, paneIdx);
            g_cv.Fill(R(c.right - 0.5f, top + 9, c.right + 0.5f, top + kHeaderH - 9),
                      HotP(ZHeaderDiv, c.col, paneIdx) || (g_drag == Drag::Column && g_dragIdx == c.col) ? g_theme.accent : g_theme.divider);
        }
    }
    g_rt->PopAxisAlignedClip();
    g_cv.Fill(R(left, t.rcHeader.bottom - 0.5f, right, t.rcHeader.bottom + 0.5f), g_theme.divider);
    AddZone(ZList, -1, t.rcList, paneIdx);

    g_rt->PushAxisAlignedClip(t.rcList, D2D1_ANTIALIAS_MODE_ALIASED);
    // drop target: the folder shown in this pane
    if (!g_dropFolder.empty() && !t.path.empty() && _wcsicmp(g_dropFolder.c_str(), t.path.c_str()) == 0)
        g_cv.StrokeRound(R(t.rcList.left + 2, t.rcList.top + 2, t.rcList.right - 2, t.rcList.bottom - 2), 8, g_theme.accent, 2);
    if (t.view.empty()) EmptyState(t);

    if (t.viewMode == ViewGrid) {
        PaintGrid(t, paneIdx, active);
    } else {
        // rows (virtualized: only the visible range is drawn)
        float rh = RowH();
        int first = std::max(0, (int)std::floor(t.scroll / rh));
        int last = std::min((int)t.view.size() - 1, (int)std::ceil((t.scroll + ListViewH(t)) / rh));
        float rowRight = t.cols.empty() ? t.rcList.right - 14 : t.cols.back().right + 6;
        rowRight = std::min(t.rcList.right - 14, std::max(rowRight, t.rcList.left + 300));
        t.rowRight = rowRight;
        if (active) g_rcRenameBox = R(0, 0, 0, 0);
        float iconSz = g_config.iconSize == L"large" ? 24.0f : g_config.iconSize == L"small" ? 14.0f : 16.0f;
        for (int v = first; v <= last; ++v) {
            int idx = t.view[v];
            const Item& it = t.items[idx];
            std::wstring full = ItemPath(t, it);
            float y = t.rcList.top + v * rh - t.scroll;
            D2D1_RECT_F row = R(t.rcList.left + 6, y + 1, rowRight, y + rh - 1);
            bool sel = t.sel[idx] != 0;
            if (sel) g_cv.FillRound(row, 6, Alpha(g_theme.accent, listFocus ? 0.24f : 0.11f));
            else if (g_hotRow == v && g_hotPane == paneIdx && g_drag == Drag::None) g_cv.FillRound(row, 6, g_theme.hover);
            if (!g_dropFolder.empty() && it.isDir() && _wcsicmp(full.c_str(), g_dropFolder.c_str()) == 0)
                g_cv.StrokeRound(row, 6, g_theme.accent, 2);
            else if (v == t.focus && listFocus && !sel) g_cv.StrokeRound(row, 6, Alpha(g_theme.accent, 0.45f), 1);
            float ry = y + rh / 2;
            float fade = (it.attrs & FILE_ATTRIBUTE_HIDDEN) ? 0.55f : 1.0f;
            D2D1_COLOR_F tc = Alpha(g_theme.text, fade), sc = Alpha(g_theme.subtext, fade);
            for (auto& c : t.cols) {
                D2D1_RECT_F cell = R(c.left + 10, y, c.right - 10, y + rh);
                switch (c.col) {
                    case ColName: {
                        DrawTagDot(full, c.left + 2, ry);
                        DrawItemIcon(it.icon, c.left + 8 + (16 - iconSz) / 2, ry, iconSz, fade);
                        D2D1_RECT_F nr = R(c.left + 34, y, c.right - 8, y + rh);
                        if (active && g_focus == Focus::Rename && idx == g_renameItem) {
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
                    case ColSize: {
                        if (thisPC) { g_cv.Text(it.total ? FormatSize(it.total) : L"", g_fRight, cell, sc); break; }
                        bool known = true;
                        uint64_t sz = it.isDir() ? DirSize(t, it, known) : it.size;
                        if (!known) {
                            if (it.isDir() && SizesAllowed(full)) g_cv.Text(L"…", g_fRight, cell, Alpha(sc, 0.6f));
                            break;
                        }
                        g_cv.Text(FormatSize(sz), g_fRight, R(cell.left, y, cell.right, y + rh - (t.maxSize ? 3 : 0)), sc);
                        if (t.maxSize && sz) {
                            // small size bar relative to the biggest item here
                            float frac = (float)((double)sz / (double)t.maxSize);
                            float bw = std::min(48.0f, cell.right - cell.left);
                            D2D1_RECT_F bar = R(cell.right - bw, y + rh - 6, cell.right, y + rh - 4);
                            g_cv.FillRound(bar, 1, Alpha(g_theme.text, 0.08f));
                            g_cv.FillRound(R(bar.right - bw * std::clamp(frac, 0.03f, 1.0f), bar.top, bar.right, bar.bottom), 1,
                                           Alpha(g_theme.accent, 0.75f));
                        }
                        break;
                    }
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
                        if (locationCol) g_cv.Text(PathParent(it.path), g_fUi, cell, sc);
                        else g_cv.Text(t.types[it.type], g_fUi, cell, sc);
                        break;
                }
            }
        }
    }
    // marquee
    if (active && g_drag == Drag::Marquee && g_dragMoved) {
        D2D1_RECT_F m = R(std::min(g_marqueeA.x, g_marqueeB.x), std::min(g_marqueeA.y, g_marqueeB.y) - t.scroll + t.rcList.top,
                          std::max(g_marqueeA.x, g_marqueeB.x), std::max(g_marqueeA.y, g_marqueeB.y) - t.scroll + t.rcList.top);
        g_cv.Fill(m, Alpha(g_theme.accent, 0.15f));
        g_cv.StrokeRound(m, 0, Alpha(g_theme.accent, 0.7f), 1);
    }
    // overlay scrollbar
    D2D1_RECT_F thumb;
    if (ScrollThumb(t, thumb)) {
        AddZone(ZScrollbar, -1, ScrollTrack(t), paneIdx);
        bool hot = HotP(ZScrollbar, -1, paneIdx) || (active && g_drag == Drag::Scrollbar);
        if (!hot) thumb.left += 4;
        g_cv.FillRound(thumb, (thumb.right - thumb.left) / 2, Alpha(g_theme.text, hot ? 0.38f : 0.22f));
    }
    g_rt->PopAxisAlignedClip();
    // dim the inactive pane slightly so the active one stands out
    if (dual && !active) g_cv.Fill(area, Alpha(g_theme.bg, 0.12f));
}

static void PaintPreview(const D2D1_RECT_F& area);    // preview panel section
static void PaintQueuePanel();                         // transfer queue section

static float SidebarW() { return g_config.showSidebar ? g_config.sidebarWidth : 0.0f; }
static float StatusH() { return g_config.showStatusBar ? kStatusH : 0.0f; }
static float PreviewW() { return g_config.showPreview ? std::min(g_config.previewWidth, g_W * 0.45f) : 0.0f; }

static void PaintPanes() {
    float left = SidebarW(), top = kTitleH + kToolH, bottom = g_H - StatusH();
    float right = g_W - PreviewW();
    Tab& tb = TB();
    if (!DualMode()) {
        Pane& p = tb.pane[tb.active];
        PaintPane(p, tb.active, R(left, top, right, bottom), true);
        tb.pane[1 - tb.active].rcPane = tb.pane[1 - tb.active].rcList = tb.pane[1 - tb.active].rcHeader = R(0, 0, 0, 0);
    } else {
        float mid = left + (right - left) * g_config.paneRatio;
        PaintPane(tb.pane[0], 0, R(left, top, mid - 3, bottom), tb.active == 0);
        PaintPane(tb.pane[1], 1, R(mid + 3, top, right, bottom), tb.active == 1);
        D2D1_RECT_F split = R(mid - 3, top, mid + 3, bottom);
        g_cv.Fill(split, SidebarColor());
        AddZone(ZPaneSplitter, -1, split);
        g_cv.Fill(R(mid - 0.5f, top, mid + 0.5f, bottom), Hot(ZPaneSplitter) || g_drag == Drag::PaneSplit ? Alpha(g_theme.accent, 0.7f) : g_theme.divider);
    }
    if (PreviewW() > 0) PaintPreview(R(right, top, g_W, bottom));
}

// ============================================================ status bar
static void PaintStatus() {
    float top = g_H - kStatusH;
    D2D1_RECT_F r = R(0, top, g_W, g_H);
    g_cv.Fill(r, ChromeColor());
    g_cv.Fill(R(0, top, g_W, top + 0.5f), g_theme.divider);
    Pane& t = T();
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
            else if (t.searchIndex) left += L"   ·   from Strata's index";
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
        if (!t.archive.empty()) left += L"   ·   inside " + PathLeaf(t.archive) + L" (read-only)";
        size_t hiddenCount = 0;
        if (!g_config.showHidden) for (auto& it : t.items) if (ItemHidden(it)) ++hiddenCount;
        if (hiddenCount) left += L"   ·   " + WithCommas(hiddenCount) + L" hidden (Ctrl+H)";
    }
    g_cv.Text(left, g_fSmall, R(14, top, g_W * 0.6f, g_H), g_statusMsg.empty() ? g_theme.subtext : g_theme.text);
    // right side: transfer queue status when there is any, otherwise keyboard hints
    auto jobs = QueueSnapshot();
    const JobInfo* cur = nullptr;
    size_t pending = 0;
    for (auto& j : jobs) {
        if (j.state == JobState::Running || j.state == JobState::Paused || j.state == JobState::Scanning) { if (!cur) cur = &j; }
        if (j.state == JobState::Queued) ++pending;
    }
    if (cur || !jobs.empty()) {
        std::wstring q;
        float frac = -1;
        if (cur) {
            q = cur->state == JobState::Scanning ? L"Preparing transfer…" : cur->state == JobState::Paused ? L"Paused" :
                (cur->kind == JobKind::Copy ? L"Copying " : L"Moving ") + WithCommas(cur->doneFiles) + L" of " + WithCommas(cur->totalFiles);
            if (cur->state == JobState::Running && cur->bytesPerSec > 0)
                q += L"   ·   " + FormatSize((uint64_t)cur->bytesPerSec) + L"/s" + (cur->etaSec >= 0 ? L"   ·   " + FormatDuration(cur->etaSec) + L" left" : L"");
            if (pending) q += L"   ·   +" + std::to_wstring(pending) + L" queued";
            if (cur->totalBytes) frac = (float)((double)cur->doneBytes / (double)cur->totalBytes);
        } else {
            q = L"Transfers done";
        }
        float tw = MeasureWidth(q, g_fSmall) + 24 + (frac >= 0 ? 70 : 0);
        D2D1_RECT_F zr = R(g_W - 14 - tw, top + 3, g_W - 8, g_H - 3);
        AddZone(ZQueueStatus, -1, zr);
        if (Hot(ZQueueStatus) || g_queueOpen) g_cv.FillRound(zr, 5, g_theme.hover);
        float x0 = zr.left + 8;
        if (frac >= 0) {
            D2D1_RECT_F bar = R(x0, (top + g_H) / 2 - 2, x0 + 60, (top + g_H) / 2 + 2);
            g_cv.FillRound(bar, 2, g_theme.surfaceStrong);
            g_cv.FillRound(R(bar.left, bar.top, bar.left + 60 * std::clamp(frac, 0.02f, 1.0f), bar.bottom), 2, g_theme.accent);
            x0 += 70;
        }
        g_cv.Text(q, g_fSmall, R(x0, top, zr.right - 6, g_H), g_theme.text);
        return;
    }
    std::wstring hints = DualMode() ? L"Tab switch pane   ·   F5 copy   ·   F6 move   ·   Ctrl+K search   ·   F2 rename"
                                    : L"Ctrl+K search   ·   Ctrl+L path   ·   F2 rename   ·   Ctrl+T new tab";
    g_cv.Text(hints, g_fSmallRight, R(g_W * 0.4f, top, g_W - 14, g_H), Alpha(g_theme.subtext, 0.75f));
}

// ============================================================ transfer queue panel
static void PaintQueuePanel() {
    if (!g_queueOpen) return;
    auto jobs = QueueSnapshot();
    if (jobs.empty()) { g_queueOpen = false; return; }
    size_t show = std::min<size_t>(jobs.size(), 6);
    float w = std::min(460.0f, g_W - 40), rowH = 64, headH = 40, footH = 36;
    float h = headH + show * rowH + footH;
    float right = g_W - 12, bottom = g_H - StatusH() - 10;
    D2D1_RECT_F card = R(right - w, bottom - h, right, bottom);
    // shadow + card
    g_cv.FillRound(R(card.left - 1, card.top + 2, card.right + 1, card.bottom + 4), 12, Rgb(0x000000, 0.25f));
    g_cv.FillRound(card, 10, g_theme.surface);
    g_cv.StrokeRound(card, 10, g_theme.divider, 1);
    AddZone(ZNone, -1, card);   // swallow clicks on the card background
    g_cv.Text(L"Transfers", g_fUiSemi, R(card.left + 16, card.top, card.right - 50, card.top + headH), g_theme.text);
    D2D1_RECT_F close = R(card.right - 38, card.top + 6, card.right - 10, card.top + headH - 6);
    AddZone(ZQueueClose, -1, close);
    if (Hot(ZQueueClose)) g_cv.FillRound(close, 5, g_theme.hover);
    g_cv.Glyph(0xE711, g_fIconSmall, close, g_theme.subtext);
    float y = card.top + headH;
    for (size_t k = 0; k < show; ++k) {
        const JobInfo& j = jobs[jobs.size() - show + k];   // newest at the bottom
        D2D1_RECT_F row = R(card.left + 10, y, card.right - 10, y + rowH);
        g_cv.Fill(R(row.left + 6, row.top, row.right - 6, row.top + 0.5f), g_theme.divider);
        bool active = j.state == JobState::Running || j.state == JobState::Paused || j.state == JobState::Scanning || j.state == JobState::Queued;
        g_cv.Text(j.title, g_fUi, R(row.left + 6, row.top + 6, row.right - 80, row.top + 28), g_theme.text);
        std::wstring sub;
        switch (j.state) {
            case JobState::Queued: sub = L"Waiting"; break;
            case JobState::Scanning: sub = L"Counting files…"; break;
            case JobState::Paused: sub = L"Paused — " + FormatSize(j.doneBytes) + L" of " + FormatSize(j.totalBytes); break;
            case JobState::Running:
                sub = FormatSize(j.doneBytes) + L" of " + FormatSize(j.totalBytes);
                if (j.bytesPerSec > 0) sub += L"  ·  " + FormatSize((uint64_t)j.bytesPerSec) + L"/s";
                if (j.etaSec >= 0) sub += L"  ·  " + FormatDuration(j.etaSec) + L" left";
                if (j.verify) sub += L"  ·  verifying";
                break;
            case JobState::Done: sub = j.errors ? L"Done with " + std::to_wstring(j.errors) + L" problem(s)" : L"Done — " + FormatSize(j.totalBytes); break;
            case JobState::Failed: sub = L"Failed" + (j.lastError.empty() ? L"" : L" — " + j.lastError.substr(0, j.lastError.find(L'\n'))); break;
            case JobState::Cancelled: sub = L"Cancelled"; break;
        }
        if (!j.current.empty() && j.state == JobState::Running) sub += L"  ·  " + j.current;
        g_cv.Text(sub, g_fSmall, R(row.left + 6, row.top + 26, row.right - 80, row.top + 44), g_theme.subtext);
        float frac = j.totalBytes ? (float)((double)j.doneBytes / (double)j.totalBytes) : (j.state == JobState::Done ? 1.0f : 0.0f);
        D2D1_RECT_F bar = R(row.left + 6, row.top + 48, row.right - 80, row.top + 52);
        g_cv.FillRound(bar, 2, g_theme.surfaceStrong);
        D2D1_COLOR_F bc = j.state == JobState::Failed || j.errors ? Rgb(0xF5A524) : j.state == JobState::Cancelled ? g_theme.subtext : g_theme.accent;
        g_cv.FillRound(R(bar.left, bar.top, bar.left + (bar.right - bar.left) * std::clamp(frac, 0.0f, 1.0f), bar.bottom), 2, bc);
        if (active) {
            D2D1_RECT_F pb = R(row.right - 72, row.top + 16, row.right - 40, row.top + 44);
            D2D1_RECT_F cb = R(row.right - 36, row.top + 16, row.right - 4, row.top + 44);
            AddZone(ZQueueJobPause, j.id, pb);
            AddZone(ZQueueJobCancel, j.id, cb);
            if (Hot(ZQueueJobPause, j.id)) g_cv.FillRound(pb, 5, g_theme.hover);
            if (Hot(ZQueueJobCancel, j.id)) g_cv.FillRound(cb, 5, g_theme.hover);
            g_cv.Glyph(j.state == JobState::Paused ? 0xE768 : 0xE769, g_fIcon, pb, g_theme.text);
            g_cv.Glyph(0xE711, g_fIcon, cb, g_theme.text);
        }
        y += rowH;
    }
    D2D1_RECT_F clear = R(card.left + 12, card.bottom - footH + 4, card.left + 140, card.bottom - 6);
    AddZone(ZQueueClear, -1, clear);
    if (Hot(ZQueueClear)) g_cv.FillRound(clear, 5, g_theme.hover);
    g_cv.Text(L"Clear finished", g_fSmall, R(clear.left + 8, clear.top, clear.right, clear.bottom), g_theme.accent);
}

static void Paint() {
    if (!EnsureDevice()) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_W = Px2Dip(rc.right);
    g_H = Px2Dip(rc.bottom);
    if (!g_tabs.empty() && g_config.showPreview) PreviewRefresh(false);   // follows the selection
    g_zones.clear();
    g_rt->BeginDraw();
    g_rt->Clear(g_theme.backdrop ? D2D1::ColorF(0, 0, 0, 0) : g_theme.bottomBg);
    if (!g_tabs.empty()) {
        PaintToolbar();
        if (g_config.showSidebar) PaintSidebar();
        else g_rcSidebar = R(0, 0, 0, 0);
        PaintPanes();
        if (g_config.showStatusBar) PaintStatus();
        PaintQueuePanel();
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
    if (k == ZSideTag || k == ZSideSaved) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Open");
        AppendMenuW(m, MF_STRING, 2, L"Open in new tab");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 3, k == ZSideTag ? L"Edit tags in Settings…" : L"Remove saved search");
        UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 1) { if (k == ZSideTag) ShowTag(T(), g_config.tags[idx].id); else RunSavedSearch(idx, false); }
        if (cmd == 2) {
            AddTab(L"", true, g_active + 1);
            if (k == ZSideTag) ShowTag(T(), g_config.tags[idx].id); else RunSavedSearch(idx, false);
        }
        if (cmd == 3) {
            if (k == ZSideTag) OpenSettings(PageTags);
            else { g_config.savedSearches.erase(g_config.savedSearches.begin() + idx); ConfigSave(); }
        }
        Redraw();
        return;
    }
    std::wstring path;
    if (k == ZSidePin) path = g_config.pins[idx].path;
    else if (k == ZSideDrive) path = g_drives[idx].root;
    else if (k == ZSideRecent) path = g_config.recentFolders[idx];
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
    } else if (k == ZSideRecent) {
        AppendMenuW(m, MF_STRING, 14, L"Pin to sidebar");
        AppendMenuW(m, MF_STRING, 15, L"Remove from recent");
        AppendMenuW(m, MF_STRING, 16, L"Clear recent folders");
    } else {
        AppendMenuW(m, MF_STRING, 20, L"Properties");
    }
    AppendMenuW(m, MF_STRING | (DualMode() ? 0 : MF_GRAYED), 5, L"Open in other pane");
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
        case 14: TogglePin(path); break;
        case 15: g_config.recentFolders.erase(g_config.recentFolders.begin() + idx); ConfigSave(); break;
        case 16: g_config.recentFolders.clear(); ConfigSave(); break;
        case 5: Navigate(OtherPane(), path); break;
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
        case 1: AddTab(g_tabs[idx].pane[g_tabs[idx].active].path, true, idx + 1); break;
        case 2: {
            SessionTab st = TabState(g_tabs[idx]);
            int at = AddTab(st.left, true, idx + 1, &st.right, st.activePane);
            for (int p = 0; p < 2; ++p) { g_tabs[at].pane[p].back = g_tabs[idx].pane[p].back; g_tabs[at].pane[p].fwd = g_tabs[idx].pane[p].fwd; }
            break;
        }
        case 3: CloseTab(idx); break;
        case 4: {
            Tab keep = std::move(g_tabs[idx]);
            for (int i = 0; i < (int)g_tabs.size(); ++i) if (i != idx) g_closedTabs.push_back(TabState(g_tabs[i]));
            g_tabs.clear();
            g_tabs.push_back(std::move(keep));
            g_active = 0;
            ActivateTab(0);
            break;
        }
        case 5:
            for (int i = (int)g_tabs.size() - 1; i > idx; --i) { g_closedTabs.push_back(TabState(g_tabs[i])); g_tabs.erase(g_tabs.begin() + i); }
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
    AppendMenuW(m, MF_STRING | (g_undo.empty() ? MF_GRAYED : 0), 3, L"Undo\tCtrl+Z");
    AppendMenuW(m, MF_STRING | (QueueSnapshot().empty() ? MF_GRAYED : 0), 4, L"Transfers");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (DualMode() ? MF_CHECKED : 0), 30, L"Dual pane\tCtrl+\\");
    AppendMenuW(m, MF_STRING | (T().viewMode == ViewGrid ? MF_CHECKED : 0), 31, L"Grid view\tCtrl+G");
    AppendMenuW(m, MF_STRING | (g_config.showPreview ? MF_CHECKED : 0), 32, L"Preview panel\tCtrl+P");
    AppendMenuW(m, MF_STRING | (g_config.showSidebar ? MF_CHECKED : 0), 33, L"Sidebar\tCtrl+B");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_config.showHidden ? MF_CHECKED : 0), 10, L"Show hidden items\tCtrl+H");
    AppendMenuW(m, MF_STRING | (g_config.showExtensions ? MF_CHECKED : 0), 11, L"Show file extensions");
    AppendMenuW(m, MF_STRING | (g_config.foldersFirst ? MF_CHECKED : 0), 12, L"Folders first");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)theme, L"Theme");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)back, L"Background");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)density, L"Row height");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 23, L"Settings…\tCtrl+,");
    AppendMenuW(m, MF_STRING, 24, L"Check for updates");
    AppendMenuW(m, MF_STRING, 22, L"About Strata");
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 100 && cmd < 100 + (UINT)kThemePresetCount) { g_config.theme = kThemePresets[cmd - 100].id; ConfigSave(); ApplyTheme(); return; }
    if (cmd >= 200 && cmd < 203) { g_config.backdrop = backs[cmd - 200]; ConfigSave(); ApplyTheme(); return; }
    switch (cmd) {
        case 1: AddTab(T().path, true, g_active + 1); break;
        case 2: ReopenClosedTab(); break;
        case 10: g_config.showHidden = !g_config.showHidden; ConfigSave(); for (auto& tb_ : g_tabs) for (auto& t : tb_.pane) BuildView(t); break;
        case 11: g_config.showExtensions = !g_config.showExtensions; ConfigSave(); break;
        case 12: g_config.foldersFirst = !g_config.foldersFirst; ConfigSave(); for (auto& tb_ : g_tabs) for (auto& t : tb_.pane) BuildView(t); break;
        case 20: g_config.restoreSession = !g_config.restoreSession; ConfigSave(); break;
        case 21: Navigate(T(), DataDir()); break;
        case 22: OpenSettings(PageAbout); break;
        case 23: OpenSettings(-1); break;
        case 24: AutoUpdateCheckNow(); g_manualUpdateCheck = true; SetStatus(L"Checking for updates…"); break;
        case 3: Undo(); break;
        case 4: g_queueOpen = true; g_queueTouched = true; break;
        case 30: ToggleDual(); break;
        case 31: ToggleGrid(); break;
        case 32: TogglePreview(); break;
        case 33: g_config.showSidebar = !g_config.showSidebar; ConfigSave(); break;
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

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
enum ItemCmd { IC_OpenLocation = 20, IC_OtherPane = 21, IC_CalcSize = 22, IC_SaveSearch = 23, IC_CopyToOther = 24, IC_MoveToOther = 25,
               IC_Extract = 26, IC_TagNone = 40, IC_TagFirst = 41, IC_OpenTab = 1, IC_Pin, IC_CopyPath, IC_Terminal, IC_NewFolder, IC_NewText, IC_Paste, IC_Refresh, IC_Props,
               IC_SelectAll, IC_OpenWith };
}  // namespace

static void ListContextMenu(POINT screenPt, bool onItem) {
    Pane& t = T();
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
        bool inArchive = !ArchiveOf(t.path).empty();
        if (paths.size() == 1 && t.focus >= 0 && t.focus < (int)t.view.size()) singleDir = t.items[t.view[t.focus]].isDir();
        if (singleDir) {
            extra.push_back({IC_OpenTab, L"Open in new tab"});
            if (DualMode()) extra.push_back({IC_OtherPane, L"Open in other pane"});
            if (!inArchive) {
                extra.push_back({IC_Pin, IsPinned(paths[0]) ? L"Unpin from sidebar" : L"Pin to sidebar"});
                extra.push_back({IC_Terminal, L"Open terminal here"});
                if (SizesAllowed(paths[0])) extra.push_back({IC_CalcSize, L"Calculate size"});
            }
        }
        if (paths.size() == 1 && !singleDir && IsArchiveFile(paths[0])) extra.push_back({IC_Extract, L"Extract here"});
        if (DualMode() && !OtherPane().path.empty() && !OtherPane().searchActive && OtherPane().tagFilter.empty()) {
            extra.push_back({IC_CopyToOther, L"Copy to other pane\tF5"});
            if (!inArchive) extra.push_back({IC_MoveToOther, L"Move to other pane\tF6"});
        }
        extra.push_back({IC_CopyPath, paths.size() > 1 ? L"Copy paths\tCtrl+Shift+C" : L"Copy path\tCtrl+Shift+C"});
        // Tag submenu
        HMENU tagMenu = nullptr;
        if (!inArchive) {
            tagMenu = CreatePopupMenu();
            std::wstring cur = TagOf(paths[0]);
            for (int i = 0; i < (int)g_config.tags.size(); ++i)
                AppendMenuW(tagMenu, MF_STRING | (cur == g_config.tags[i].id ? MF_CHECKED : 0), kMenuExtraBase + IC_TagFirst + i, g_config.tags[i].name.c_str());
            AppendMenuW(tagMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(tagMenu, MF_STRING | (cur.empty() ? MF_GRAYED : 0), kMenuExtraBase + IC_TagNone, L"No tag");
        }
        int result = ShowItemContextMenu(g_hwnd, screenPt, t.path, paths, extra, [&](const std::wstring& verb) {
            if (verb == L"rename") { BeginRename(); return true; }
            if ((verb == L"open" || verb == L"explore" || verb == L"opennewwindow" || verb == L"opennewprocess") && paths.size() == 1 &&
                IsDirectory(paths[0])) {
                if (verb == L"open" || verb == L"explore") Navigate(T(), paths[0]);
                else OpenFolderInNewTab(paths[0], true);
                return true;
            }
            if (verb == L"pintohome") { TogglePin(paths[0]); return true; }
            if (verb == L"delete") { DoDelete(GetKeyState(VK_SHIFT) < 0); return true; }   // goes through Strata so it can be undone
            return false;
        }, tagMenu, L"Tag");
        switch (result) {
            case IC_OpenLocation: Navigate(T(), PathParent(paths[0]), true, PathLeaf(paths[0])); break;
            case IC_OpenTab: OpenFolderInNewTab(paths[0], true); break;
            case IC_Pin: TogglePin(paths[0]); break;
            case IC_Terminal: OpenTerminal(paths[0]); break;
            case IC_CopyPath: CopyPathsToClipboard(paths); break;
            case IC_OtherPane: Navigate(OtherPane(), paths[0]); WatchActive(); break;
            case IC_CalcSize: SizesRequest({paths[0]}, true); SetStatus(L"Calculating the size of " + PathLeaf(paths[0]) + L"…"); break;
            case IC_CopyToOther: TransferToOtherPane(JobKind::Copy); break;
            case IC_MoveToOther: TransferToOtherPane(JobKind::Move); break;
            case IC_Extract: {
                // copy everything out of the archive into a folder named after it
                std::wstring arc = paths[0], leaf = PathLeaf(arc);
                size_t dot = leaf.find_last_of(L'.');
                std::wstring folder = PathJoin(PathParent(arc), UniqueName(PathParent(arc), dot == std::wstring::npos ? leaf : leaf.substr(0, dot), L""));
                if (CreateDirectoryW(folder.c_str(), nullptr)) {
                    std::vector<std::wstring> inner;
                    for (auto& n : ListArchiveNames(arc)) inner.push_back(PathJoin(arc, n));
                    ShellCopyOut(g_hwnd, inner, folder);
                    SetStatus(L"Extracting " + leaf + L"…");
                }
                break;
            }
            default:
                if (result == IC_TagNone) { for (auto& p2 : paths) TagSet(p2, L""); }
                else if (result >= IC_TagFirst && result < IC_TagFirst + (int)g_config.tags.size()) {
                    for (auto& p2 : paths) TagSet(p2, g_config.tags[result - IC_TagFirst].id);
                    SetStatus(L"Tagged " + std::to_wstring(paths.size()) + (paths.size() == 1 ? L" item " : L" items ") + g_config.tags[result - IC_TagFirst].name);
                }
                break;
        }
        Redraw();
        return;
    }
    // background menu
    bool real = !t.path.empty() && ArchiveOf(t.path).empty() && t.tagFilter.empty();
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
    if (t.searchActive) AppendMenuW(m, MF_STRING, IC_SaveSearch, L"Save search to sidebar…");
    if (real && SizesAllowed(t.path)) AppendMenuW(m, MF_STRING, IC_CalcSize, L"Recalculate folder sizes");
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
        case IC_SaveSearch: SaveCurrentSearch(); break;
        case IC_CalcSize: {
            std::vector<std::wstring> dirs;
            for (int v : t.view) if (t.items[v].isDir()) dirs.push_back(ItemPath(t, t.items[v]));
            SizesRequest(dirs, true);
            break;
        }
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
    Pane& t = T();
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
static void MoveFocus(Pane& t, int to, bool shift, bool ctrl) {
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
    Pane& t = T();
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
    Pane& t = T();
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
            case 'H': g_config.showHidden = !g_config.showHidden; ConfigSave(); for (auto& tb_ : g_tabs) for (auto& pn : tb_.pane) BuildView(pn);
                      SetStatus(g_config.showHidden ? L"Showing hidden items" : L"Hiding hidden items"); return true;
            case 'Z': if (!shift) { Undo(); return true; } break;
            case 'G': ToggleGrid(); return true;
            case 'P': TogglePreview(); return true;
            case 'B': g_config.showSidebar = !g_config.showSidebar; ConfigSave(); Redraw(); return true;
            case VK_OEM_5: ToggleDual(); return true;          // Ctrl+backslash
            case VK_OEM_COMMA: OpenSettings(-1); return true;  // Ctrl+,
            case 'J': g_queueOpen = !g_queueOpen; g_queueTouched = true; Redraw(); return true;
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
    if (vk == VK_F5 && !ctrl && !alt && (!DualMode() || g_focus != Focus::List)) { Load(t, true); return true; }
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
            Pane& t = T();
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
        Pane& t = T();
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
    Pane& t = T();
    bool grid = t.viewMode == ViewGrid;
    int step = grid ? std::max(1, t.gridCols) : 1;
    int page = std::max(1, (int)(ListViewH() / ItemRowH(t)) - 1) * step;
    int cur = t.focus < 0 ? -1 : t.focus;
    switch (vk) {
        case VK_DOWN: MoveFocus(t, cur < 0 ? 0 : std::min(cur + step, (int)t.view.size() - 1), shift, ctrl); return;
        case VK_UP: MoveFocus(t, cur < 0 ? 0 : (cur - step < 0 ? cur : cur - step), shift, ctrl); return;
        case VK_RIGHT: if (grid) MoveFocus(t, cur + 1, shift, ctrl); return;
        case VK_LEFT: if (grid) MoveFocus(t, cur < 0 ? 0 : cur - 1, shift, ctrl); return;
        case VK_NEXT: MoveFocus(t, cur + page, shift, ctrl); return;
        case VK_PRIOR: MoveFocus(t, cur - page, shift, ctrl); return;
        case VK_TAB:
            // Tab switches the active pane (dual-pane mode)
            if (DualMode() && !ctrl && !alt) { SetActivePane(1 - TB().active); }
            return;
        case VK_F5: TransferToOtherPane(JobKind::Copy); return;
        case VK_F6: TransferToOtherPane(JobKind::Move); return;
        case VK_HOME: MoveFocus(t, 0, shift, ctrl); return;
        case VK_END: MoveFocus(t, (int)t.view.size() - 1, shift, ctrl); return;
        case VK_SPACE:
            if (ctrl && t.focus >= 0) { t.sel[t.view[t.focus]] ^= 1; Redraw(); return; }
            if (g_typeBuf.empty() || GetTickCount() - g_typeTime > 900) QuickPreview();   // Space = quick preview
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
            if (t.focus >= 0 && !grid) {
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
static Pane& PaneAt(int i) { return TB().pane[i < 0 ? TB().active : i]; }

static void UpdateHover(float x, float y) {
    Zone z = HitZoneTop(x, y);
    int row = (z.kind == ZList && z.pane >= 0) ? ItemAt(PaneAt(z.pane), x, y) : -1;
    if (z.kind != g_hotKind || z.idx != g_hotIdx || row != g_hotRow || z.pane != g_hotPane) {
        g_hotKind = z.kind;
        g_hotIdx = z.idx;
        g_hotRow = row;
        g_hotPane = z.pane;
        Redraw();
    }
}

static void SetCursorFor(ZoneKind k) {
    LPCWSTR c = IDC_ARROW;
    if (k == ZSideSplitter || k == ZHeaderDiv || k == ZPaneSplitter || k == ZPreviewSplitter) c = IDC_SIZEWE;
    else if (k == ZSearch || (k == ZPathBox && g_focus == Focus::Path)) c = IDC_IBEAM;
    SetCursor(LoadCursorW(nullptr, c));
}

static void ApplyMarquee() {
    Pane& t = T();
    float y0 = std::min(g_marqueeA.y, g_marqueeB.y), y1 = std::max(g_marqueeA.y, g_marqueeB.y);
    t.sel = g_marqueeBase;
    if (t.sel.size() != t.items.size()) t.sel.assign(t.items.size(), 0);
    if (t.viewMode == ViewGrid) {
        float x0 = std::min(g_marqueeA.x, g_marqueeB.x), x1 = std::max(g_marqueeA.x, g_marqueeB.x);
        int r0 = (int)std::floor((y0 - 4) / t.tileH), r1 = (int)std::floor((y1 - 4) / t.tileH);
        int c0 = (int)std::floor((x0 - t.rcList.left - 8) / t.tileW), c1 = (int)std::floor((x1 - t.rcList.left - 8) / t.tileW);
        c0 = std::max(0, c0);
        c1 = std::min(t.gridCols - 1, c1);
        for (int r = std::max(0, r0); r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                int v = r * t.gridCols + c;
                if (v >= 0 && v < (int)t.view.size()) { t.sel[t.view[v]] = 1; t.focus = v; }
            }
        return;
    }
    float rh = RowH();
    int a = (int)std::floor(y0 / rh), b = (int)std::floor(y1 / rh);
    for (int v = std::max(0, a); v <= b && v < (int)t.view.size(); ++v) t.sel[t.view[v]] = 1;
    if (b >= 0 && b < (int)t.view.size()) t.focus = b;
}

static bool g_pressWasSelected = false;

static void OnLButtonDown(float x, float y, bool dbl) {
    SetCapture(g_hwnd);
    g_drag = Drag::None;
    g_dragMoved = false;
    g_dragStartX = x;
    g_dragStartY = y;
    Zone z = HitZoneTop(x, y);
    g_pressKind = z.kind;
    // clicking in the other pane makes it the active one
    if (z.pane >= 0 && z.pane != TB().active) SetActivePane(z.pane);
    Pane& t = T();
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
        case ZViewSwitch: case ZUpdatePill: case ZQueueStatus: case ZQueueJobPause: case ZQueueJobCancel: case ZQueueClear: case ZQueueClose:
        case ZPreviewButton: case ZSideTag: case ZSideSaved: case ZSideRecent: case ZSideSection: case ZPreviewToggle:
            g_drag = Drag::None;
            g_dragIdx = z.idx;
            if (z.kind == ZQueueJobPause || z.kind == ZQueueJobCancel || z.kind == ZQueueClear) g_queueTouched = true;
            return;   // acted on in button-up
        case ZPaneSplitter:
            g_drag = Drag::PaneSplit;
            g_dragOrig = g_config.paneRatio;
            return;
        case ZPreviewSplitter:
            g_drag = Drag::PreviewSplit;
            g_dragOrig = g_config.previewWidth;
            return;
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
            for (auto& c : t.cols) if (c.col == z.idx) g_dragOrig = c.right - c.left;   // start from what's on screen
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
                } else {
                    // plain click: select now (or on button-up for an already-selected row, so a drag keeps the selection)
                    g_pressWasSelected = t.sel[idx] != 0;
                    if (!g_pressWasSelected) SelectOnly(t, v);
                    t.focus = v;
                    g_pressRow = v;
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
    Pane& t = T();
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
        case Drag::PaneSplit: {
            float left = SidebarW(), right = g_W - PreviewW();
            if (right - left > 100) g_config.paneRatio = std::clamp(g_dragOrig + (x - g_dragStartX) / (right - left), 0.2f, 0.8f);
            Redraw();
            return;
        }
        case Drag::PreviewSplit:
            g_config.previewWidth = std::clamp(g_dragOrig - (x - g_dragStartX), 220.0f, std::min(700.0f, g_W * 0.45f));
            Redraw();
            return;
        case Drag::ListPress:
            if (g_dragMoved && !DriveView(t)) {
                // drag the selection out (to the other pane, a tab, the sidebar, or another app)
                auto paths = SelectedPaths(t);
                g_drag = Drag::None;
                if (GetCapture() == g_hwnd) ReleaseCapture();
                if (!paths.empty()) {
                    DWORD effect = DragFiles(g_hwnd, paths);
                    if (effect & DROPEFFECT_MOVE) ReloadPanesFor(paths);
                }
                g_pressRow = -1;
                Redraw();
            }
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
    Pane& t = T();
    if (was == Drag::Splitter || was == Drag::Column || was == Drag::PaneSplit || was == Drag::PreviewSplit) { ConfigSave(); Redraw(); return; }
    if (was == Drag::Tab || was == Drag::Scrollbar || was == Drag::Marquee || was == Drag::InputSelect) { Redraw(); return; }
    if (was == Drag::ListPress) {
        if (!g_dragMoved && g_pressWasSelected && g_pressRow >= 0 && g_pressRow < (int)t.view.size()) SelectOnly(t, g_pressRow);
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
        case ZHeader: {
            if (t.sortColumn == z.idx) t.sortDescending = !t.sortDescending;
            else { t.sortColumn = z.idx; t.sortDescending = z.idx == ColModified || z.idx == ColSize; }
            // new default for folders without their own view, and remembered for this folder
            g_config.sortColumn = t.sortColumn;
            g_config.sortDescending = t.sortDescending;
            ConfigSave();
            SaveFolderView(t);
            BuildView(t);
            break;
        }
        case ZViewSwitch:
            if (z.idx == 0) { g_config.layout = L"dual"; if (t.viewMode == ViewGrid) { t.viewMode = ViewList; SaveFolderView(t); } }
            else if (z.idx == 1) { g_config.layout = L"single"; if (t.viewMode == ViewGrid) { t.viewMode = ViewList; SaveFolderView(t); } }
            else { t.viewMode = t.viewMode == ViewGrid ? ViewList : ViewGrid; SaveFolderView(t); EnsureVisible(t, t.focus); }
            ConfigSave();
            WatchActive();
            break;
        case ZPreviewToggle: TogglePreview(); break;
        case ZUpdatePill: UpdatePillClicked(); break;
        case ZQueueStatus: g_queueOpen = !g_queueOpen; g_queueTouched = true; break;
        case ZQueueClose: g_queueOpen = false; break;
        case ZQueueClear: QueueClearFinished(); break;
        case ZQueueJobPause: {
            for (auto& j : QueueSnapshot()) if (j.id == z.idx) QueuePause(j.id, j.state != JobState::Paused);
            break;
        }
        case ZQueueJobCancel: QueueCancel(z.idx); break;
        case ZPreviewButton: PreviewButton(z.idx); break;
        case ZSideTag: ShowTag(t, g_config.tags[z.idx].id); break;
        case ZSideSaved: RunSavedSearch(z.idx, ctrl); break;
        case ZSideRecent: if (ctrl) OpenFolderInNewTab(g_config.recentFolders[z.idx], true); else Navigate(t, g_config.recentFolders[z.idx]); break;
        case ZSideSection: ToggleSidebarSection(z.idx); break;
        default: break;
    }
    Redraw();
}

static void OnMiddleClick(float x, float y) {
    Zone z = HitZoneTop(x, y);
    if (z.pane >= 0 && z.pane != TB().active) SetActivePane(z.pane);
    Pane& t = T();
    switch (z.kind) {
        case ZTab: case ZTabClose: CloseTab(z.idx); break;
        case ZSidePin: OpenFolderInNewTab(g_config.pins[z.idx].path, false); break;
        case ZSideDrive: OpenFolderInNewTab(g_drives[z.idx].root, false); break;
        case ZSideThisPC: OpenFolderInNewTab(L"", false); break;
        case ZCrumb: OpenFolderInNewTab(g_crumbs[z.idx].path, false); break;
        case ZSideRecent: OpenFolderInNewTab(g_config.recentFolders[z.idx], false); break;
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
    if (z.pane >= 0 && z.pane != TB().active) SetActivePane(z.pane);
    Pane& t = T();
    switch (z.kind) {
        case ZTab: case ZTabClose: TabMenu(z.idx, pt); break;
        case ZSidePin: case ZSideDrive: case ZSideTag: case ZSideSaved: case ZSideRecent: SidebarMenu(z.kind, z.idx, pt); break;
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
    Pane& t = T();
    if (In(g_rcSidebar, x, y)) {
        float maxS = std::max(0.0f, g_sideContentH - (g_rcSidebar.bottom - g_rcSidebar.top));
        g_sideScroll = std::clamp(g_sideScroll - delta / 120.0f * 48, 0.0f, maxS);
        Redraw();
        return;
    }
    if (PreviewW() > 0 && x >= g_W - PreviewW() && y >= kTitleH + kToolH) {
        PreviewScroll(delta);
        return;
    }
    if (y >= kTitleH + kToolH) {
        // scroll the pane under the mouse (it doesn't have to be the active one)
        Pane* target = &t;
        for (auto& pn : TB().pane) if (In(pn.rcPane, x, y)) target = &pn;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL || lines == 0) lines = 3;
        float step = target->viewMode == ViewGrid ? target->tileH : RowH() * lines;
        target->scrollTarget -= delta / 120.0f * step;
        ClampScroll(*target);
        SetTimer(g_hwnd, TIMER_SCROLL, 8, nullptr);
    } else if (In(R(0, 0, g_W, kTitleH), x, y) && g_tabs.size() > 1) {
        ActivateTab((g_active + (delta > 0 ? -1 : 1) + (int)g_tabs.size()) % (int)g_tabs.size());
    }
}

// ============================================================ async results
static void OnListing(Listing* L) {
    for (auto& tb_ : g_tabs) for (auto& t : tb_.pane) {
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
        t.archive = L->archive;
        if (t.error == ERROR_NOT_SUPPORTED && IsArchiveFile(t.path)) {
            // Windows can't browse this archive format: open it with its app instead
            std::wstring arc = t.path;
            GoBack(t);
            OpenItem(g_hwnd, arc, nullptr);
            break;
        }
        // folder sizes for the subfolders shown here (visible ones first)
        if (t.archive.empty() && SizesAllowed(t.path)) {
            std::vector<std::wstring> dirs;
            int first = std::max(0, (int)(t.scroll / RowH()));
            for (int pass = 0; pass < 2; ++pass)
                for (int v = 0; v < (int)t.view.size() && dirs.size() < 3000; ++v) {
                    bool visible = v >= first && v < first + 60;
                    if ((pass == 0) != visible) continue;
                    const Item& it = t.items[t.view[v]];
                    if (it.isDir()) dirs.push_back(PathJoin(t.path, it.name));
                }
            if (&t == &T()) SizesCancelPending();
            SizesRequest(dirs);
        }
        if (&t == &T()) {
            if (t.path.empty() || !g_folderGit.folder.empty()) g_folderGit = GitResult();
            if (!t.path.empty() && t.archive.empty()) GitInfoAsync(g_hwnd, 0, t.path);
        }
        break;
    }
    delete L;
    Redraw();
}

static void OnSearch(SearchBatch* b) {
    for (auto& tb_ : g_tabs) for (auto& t : tb_.pane) {
        if (t.id != b->tab || t.gen != b->gen || !t.searchActive) continue;
        std::map<std::wstring, int> idx;
        for (int i = 0; i < (int)t.types.size(); ++i) idx[t.types[i]] = i;
        std::vector<int> remap(b->typeNames.size());
        for (size_t i = 0; i < b->typeNames.size(); ++i) {
            auto it = idx.find(b->typeNames[i]);
            if (it == idx.end()) { remap[i] = (int)t.types.size(); t.types.push_back(b->typeNames[i]); }
            else remap[i] = it->second;
        }
        SearchQuery q = ParseQuery(t.searchQuery);
        std::wstring tagId;
        if (!q.tag.empty())
            for (auto& tg : g_config.tags) if (_wcsicmp(tg.name.c_str(), q.tag.c_str()) == 0 || tg.id == q.tag) tagId = tg.id;
        for (auto& it : b->items) {
            if (!q.tag.empty() && (tagId.empty() || TagOf(it.path) != tagId)) continue;   // tag: filter
            it.type = remap[it.type];
            t.items.push_back(std::move(it));
        }
        t.sel.resize(t.items.size(), 0);
        if (b->done) t.searchRunning = false;
        t.searchEverything = b->everything;
        t.searchIndex = b->fromIndex;
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
    for (auto& tb_ : g_tabs) for (auto& t : tb_.pane) {
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
    if (changed) for (auto& tb_ : g_tabs) for (auto& t : tb_.pane) if (t.path.empty() && t.loaded) Load(t, true);
    Redraw();
}

static void OnOpDone(OpResult* r) {
    if (!r->ok && !r->aborted && r->hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) && r->hr != E_ABORT && r->hr != (HRESULT)0x80270000L /*COPYENGINE_E_USER_CANCELLED*/)
        SetStatus(L"The operation didn't complete (" + ErrorText(HRESULT_CODE(r->hr)) + L")");
    else if (r->ok) {
        const wchar_t* what = r->kind == OpKind::Copy ? L"Copied " : r->kind == OpKind::Move ? L"Moved " :
                              r->kind == OpKind::Recycle ? L"Moved to Recycle Bin: " : L"Deleted ";
        if (!(r->kind == OpKind::Recycle && g_undoSkipNextRecycle))
            SetStatus(what + std::to_wstring(r->sources.size()) + (r->sources.size() == 1 ? L" item" : L" items") +
                      (r->kind == OpKind::Recycle ? L"   ·   Ctrl+Z to undo" : L""));
        if (r->kind == OpKind::Recycle) {
            if (!g_undoSkipNextRecycle) PushUndo({UndoRec::Recycle, {}, r->sources});
            for (auto& src : r->sources) { TagSet(src, L""); SizesInvalidate(PathParent(src)); }
        }
        if (r->kind == OpKind::Delete) for (auto& src : r->sources) { TagSet(src, L""); SizesInvalidate(PathParent(src)); }
    }
    if (r->kind == OpKind::Recycle) g_undoSkipNextRecycle = false;
    // search results and tag views: drop what's gone instead of reloading
    for (auto& tb_ : g_tabs) for (auto& pn : tb_.pane) {
        if ((pn.searchActive || !pn.tagFilter.empty()) && r->ok && r->kind != OpKind::Copy) {
            std::set<std::wstring> gone;
            for (auto& src : r->sources) gone.insert(ToLower(src));
            std::vector<Item> keep;
            for (auto& it : pn.items) if (!gone.count(ToLower(it.path))) keep.push_back(std::move(it));
            pn.items = std::move(keep);
            pn.sel.assign(pn.items.size(), 0);
            BuildView(pn);
        }
    }
    ReloadPanesFor(r->sources);
    if (!r->dest.empty()) ReloadPanesFor({PathJoin(r->dest, L"x")});
    delete r;
    ListDrivesAsync(g_hwnd);   // free space changed
}

static void OnJobDone(JobResult* r) {
    if (r->kind == JobKind::Move && !r->moved.empty()) {
        PushUndo({UndoRec::Move, r->moved, {}});
        for (auto& m : r->moved) { TagsPathMoved(m.first, m.second); SizesInvalidate(PathParent(m.first)); }
    }
    if (r->kind == JobKind::Copy && !r->created.empty()) PushUndo({UndoRec::Copy, {}, r->created});
    SizesInvalidate(r->dest);
    std::wstring msg;
    if (r->cancelled) msg = L"Transfer cancelled";
    else if (r->errors) msg = L"Finished with " + std::to_wstring(r->errors) + (r->errors == 1 ? L" problem" : L" problems") +
                              (r->skipped ? L" (" + std::to_wstring(r->skipped) + L" skipped)" : L"");
    else msg = std::wstring(r->kind == JobKind::Copy ? L"Copied " : L"Moved ") +
               std::to_wstring(r->kind == JobKind::Copy ? r->created.size() : r->moved.size()) + L" item" +
               ((r->kind == JobKind::Copy ? r->created.size() : r->moved.size()) == 1 ? L"" : L"s") + L"   ·   Ctrl+Z to undo";
    SetStatus(msg);
    // select what arrived in the destination pane
    for (auto& tb_ : g_tabs) for (auto& pn : tb_.pane) {
        if (_wcsicmp(pn.path.c_str(), r->dest.c_str()) != 0) continue;
        for (auto& c : r->created) pn.restoreSel.insert(PathLeaf(c));
        for (auto& m : r->moved) pn.restoreSel.insert(PathLeaf(m.second));
    }
    std::vector<std::wstring> touched;
    for (auto& m : r->moved) { touched.push_back(m.first); touched.push_back(m.second); }
    for (auto& c : r->created) touched.push_back(c);
    touched.push_back(PathJoin(r->dest, L"x"));
    ReloadPanesFor(touched);
    delete r;
    ListDrivesAsync(g_hwnd);
    // close the queue panel by itself once everything is finished, unless the user is looking at it
    if (!QueueBusy() && !g_queueTouched) SetTimer(g_hwnd, TIMER_QUEUECLOSE, 4000, nullptr);
    Redraw();
}

// The copy engine asks about conflicts and errors through a dialog
static void OnQueueAsk(QueueAsk* a) {
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = g_hwnd;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"Strata";
    std::wstring main, content;
    std::vector<TASKDIALOG_BUTTON> buttons;
    if (a->kind == QueueAsk::Conflict) {
        main = L"\"" + PathLeaf(a->dst) + L"\" already exists in " + PathLeaf(PathParent(a->dst));
        WIN32_FILE_ATTRIBUTE_DATA fs{}, fd{};
        GetFileAttributesExW(a->src.c_str(), GetFileExInfoStandard, &fs);
        GetFileAttributesExW(a->dst.c_str(), GetFileExInfoStandard, &fd);
        auto sz = [](const WIN32_FILE_ATTRIBUTE_DATA& f) { return FormatSize(((uint64_t)f.nFileSizeHigh << 32) | f.nFileSizeLow); };
        content = L"Incoming:  " + sz(fs) + L",  modified " + FormatDate(fs.ftLastWriteTime) + L"\n" +
                  L"Existing:   " + sz(fd) + L",  modified " + FormatDate(fd.ftLastWriteTime);
        buttons = {{100, L"Replace"}, {101, L"Skip"}, {102, L"Keep both"}, {103, L"Cancel transfer"}};
        tc.pszVerificationText = L"Do this for all conflicts";
    } else if (a->kind == QueueAsk::Error) {
        main = L"There was a problem";
        content = a->message;
        buttons = {{100, L"Try again"}, {101, L"Skip"}, {102, L"Cancel transfer"}};
        tc.pszVerificationText = L"Skip all errors in this transfer";
    } else {
        main = L"Not enough free space";
        content = a->message;
        buttons = {{100, L"Try anyway"}, {101, L"Cancel transfer"}};
    }
    tc.pszMainInstruction = main.c_str();
    tc.pszContent = content.c_str();
    tc.pszMainIcon = a->kind == QueueAsk::Conflict ? TD_INFORMATION_ICON : TD_WARNING_ICON;
    tc.cButtons = (UINT)buttons.size();
    tc.pButtons = buttons.data();
    tc.nDefaultButton = 100;
    int button = 0;
    BOOL verified = FALSE;
    if (FAILED(TaskDialogIndirect(&tc, &button, nullptr, &verified))) button = a->kind == QueueAsk::Conflict ? 101 : 101;
    if (button == IDCANCEL) button = a->kind == QueueAsk::Conflict ? 103 : a->kind == QueueAsk::Error ? 102 : 101;
    a->answer = button - 100;
    a->applyToAll = verified != FALSE;
}

// ============================================================ thumbnails (grid view)
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct ThumbEntry { ID2D1Bitmap* bmp = nullptr; int px = 0; bool pending = false; bool failed = false; DWORD used = 0; };
}  // namespace
static std::unordered_map<std::wstring, ThumbEntry> g_thumbs;   // lowercase path

static void ThumbsDiscard() {
    for (auto& kv : g_thumbs) SafeRelease(kv.second.bmp);
    g_thumbs.clear();
}

static ID2D1Bitmap* MakeBitmap(const PixelImage& img) {
    if (!g_rt || !img.w || !img.h) return nullptr;
    ID2D1Bitmap* b = nullptr;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                       (float)g_dpi, (float)g_dpi);
    g_rt->CreateBitmap(D2D1::SizeU(img.w, img.h), img.pixels.data(), img.w * 4, bp, &b);
    return b;
}

static ID2D1Bitmap* ThumbFor(const std::wstring& path, int px) {
    std::wstring key = ToLower(path);
    ThumbEntry& e = g_thumbs[key];
    e.used = GetTickCount();
    if (e.bmp && e.px >= px * 3 / 4) return e.bmp;
    if (!e.pending && !(e.failed && e.px >= px)) {
        e.pending = true;
        ThumbRequest(g_hwnd, path, std::max(64, px));
    }
    return e.bmp;
}

static void OnThumb(ThumbResult* r) {
    ThumbEntry& e = g_thumbs[ToLower(r->path)];
    e.pending = false;
    e.px = r->px;
    if (r->ok) {
        SafeRelease(e.bmp);
        e.bmp = MakeBitmap(r->img);
    } else {
        e.failed = true;
    }
    delete r;
    // keep the cache bounded: drop the least recently drawn thumbnails
    if (g_thumbs.size() > 600) {
        std::vector<std::pair<DWORD, std::wstring>> ages;
        for (auto& kv : g_thumbs) if (!kv.second.pending) ages.push_back({kv.second.used, kv.first});
        std::sort(ages.begin(), ages.end());
        for (size_t i = 0; i < ages.size() / 3; ++i) { SafeRelease(g_thumbs[ages[i].second].bmp); g_thumbs.erase(ages[i].second); }
    }
    Redraw();
}

// ============================================================ preview panel
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct PreviewState {
    uint64_t gen = 0;
    std::wstring path;           // what's being previewed
    bool isDir = false;
    bool loading = false;
    PreviewResult* res = nullptr;
    ID2D1Bitmap* bmp = nullptr;
    GitResult git;
    bool gitLoaded = false;
    float scroll = 0;
    bool handlerShown = false;
    D2D1_RECT_F contentRc{};
    bool quick = false;          // opened with Space
    bool reloadBitmap = false;
};
}  // namespace
static PreviewState g_pv;
static uint64_t g_pvGen = 1;
static IDWriteTextFormat* g_fMono = nullptr;

static void PreviewClear() {
    PreviewHandlerClose();
    g_pv.handlerShown = false;
    SafeRelease(g_pv.bmp);
    delete g_pv.res;
    g_pv.res = nullptr;
    g_pv.path.clear();
    g_pv.scroll = 0;
}

// What the preview shows: the focused item, or the current folder when nothing is selected
static void PreviewTarget(std::wstring& path, bool& isDir) {
    path.clear();
    isDir = false;
    if (g_tabs.empty()) return;
    Pane& t = T();
    if (t.focus >= 0 && t.focus < (int)t.view.size() && (SelectedCount(t) > 0)) {
        const Item& it = t.items[t.view[t.focus]];
        path = ItemPath(t, it);
        isDir = it.isDir();
        if (DriveView(t)) { path = it.path; isDir = true; }
    } else if (!t.path.empty() && !t.searchActive && t.tagFilter.empty()) {
        path = t.path;
        isDir = true;
    }
}

static void PreviewRefresh(bool force = false) {
    if (!g_config.showPreview || g_tabs.empty()) { if (!g_pv.path.empty()) PreviewClear(); return; }
    std::wstring path;
    bool isDir;
    PreviewTarget(path, isDir);
    if (!force && _wcsicmp(path.c_str(), g_pv.path.c_str()) == 0) return;
    PreviewClear();
    g_pv.path = path;
    g_pv.isDir = isDir;
    if (path.empty()) { Redraw(); return; }
    g_pv.gen = ++g_pvGen;
    g_pv.loading = true;
    g_pv.gitLoaded = false;
    if (!ArchiveOf(path).empty() && !IsArchiveFile(path)) { g_pv.loading = false; Redraw(); return; }   // inside an archive: details only
    PreviewLoadAsync(g_hwnd, g_pv.gen, path, isDir, (int)(std::max(400.0f, PreviewW()) * g_scale * 1.5f));
    if (isDir) {
        GitInfoAsync(g_hwnd, g_pv.gen, path);
        // sizes of the subfolders for "what's taking space"
        if (SizesAllowed(path)) {
            std::vector<std::wstring> subs;
            for (auto& n : ListSubfolders(path, 300, true)) subs.push_back(PathJoin(path, n));
            subs.insert(subs.begin(), path);
            SizesRequest(subs);
        }
    }
    Redraw();
}

static void OnPreview(PreviewResult* r) {
    if (r->gen != g_pv.gen) { delete r; return; }
    g_pv.loading = false;
    delete g_pv.res;
    g_pv.res = r;
    SafeRelease(g_pv.bmp);
    if (r->kind == PreviewResult::Image || r->kind == PreviewResult::Thumb) g_pv.bmp = MakeBitmap(r->img);
    if (r->kind == PreviewResult::Handler) SetTimer(g_hwnd, TIMER_PREVIEW, 200, nullptr);   // host the Windows previewer
    Redraw();
}

static void OnGit(GitResult* r) {
    if (r->gen == g_pv.gen) { g_pv.git = *r; g_pv.gitLoaded = true; }
    // the breadcrumb badge for the active folder
    if (!g_tabs.empty() && _wcsicmp(r->folder.c_str(), T().path.c_str()) == 0) { g_folderGit = *r; }
    delete r;
    Redraw();
}

static void ShowHandlerNow() {
    if (!g_pv.res || g_pv.res->kind != PreviewResult::Handler || g_pv.handlerShown) return;
    D2D1_RECT_F c = g_pv.contentRc;
    if (c.right - c.left < 50 || c.bottom - c.top < 50) return;
    RECT rc{(LONG)(c.left * g_scale), (LONG)(c.top * g_scale), (LONG)(c.right * g_scale), (LONG)(c.bottom * g_scale)};
    auto toRef = [](D2D1_COLOR_F f) { return RGB((int)(f.r * 255), (int)(f.g * 255), (int)(f.b * 255)); };
    g_pv.handlerShown = PreviewHandlerShow(g_hwnd, g_pv.path, g_pv.res->handlerClsid, rc, toRef(g_theme.surface), toRef(g_theme.text));
    if (!g_pv.handlerShown) {
        // fall back to a thumbnail
        g_pv.res->kind = PreviewResult::None;
    }
    Redraw();
}

static void PreviewBitmapsDiscard() {
    // device lost: rebuild the preview bitmap from the pixels we kept
    SafeRelease(g_pv.bmp);
    if (g_pv.res && (g_pv.res->kind == PreviewResult::Image || g_pv.res->kind == PreviewResult::Thumb)) g_pv.reloadBitmap = true;
}

static void TogglePreview() {
    g_config.showPreview = !g_config.showPreview;
    g_pv.quick = false;
    ConfigSave();
    PreviewRefresh(true);
    Redraw();
}

static void QuickPreview() {
    // Space: open the preview panel for the selection (Space again closes a quick preview)
    if (g_config.showPreview && g_pv.quick) { g_config.showPreview = false; g_pv.quick = false; PreviewRefresh(); Redraw(); return; }
    if (!g_config.showPreview) { g_config.showPreview = true; g_pv.quick = true; }
    PreviewRefresh(true);
    Redraw();
}

static void PreviewScroll(int delta) {
    g_pv.scroll = std::max(0.0f, g_pv.scroll - delta / 120.0f * 60);
    Redraw();
}

static void PreviewButton(int idx) {
    if (g_pv.path.empty()) return;
    if (idx == 0) {
        if (g_pv.isDir) Navigate(T(), g_pv.path);
        else { std::wstring target; if (!OpenItem(g_hwnd, g_pv.path, &target)) Navigate(T(), target); }
    } else if (idx == 1) {
        std::wstring folder = g_pv.isDir ? g_pv.path : PathParent(g_pv.path);
        if (!DualMode()) { g_config.layout = L"dual"; ConfigSave(); }
        Navigate(OtherPane(), folder, true, g_pv.isDir ? L"" : PathLeaf(g_pv.path));
        WatchActive();
    } else if (idx == 2) {
        SizesRequest({g_pv.path}, true);
    }
    Redraw();
}

static void PaintPreview(const D2D1_RECT_F& area) {
    g_cv.Fill(area, SidebarColor());
    D2D1_RECT_F split = R(area.left - 3, area.top, area.left + 3, area.bottom);
    AddZone(ZPreviewSplitter, -1, split);
    g_cv.Fill(R(area.left - 0.5f, area.top, area.left + 0.5f, area.bottom),
              Hot(ZPreviewSplitter) || g_drag == Drag::PreviewSplit ? Alpha(g_theme.accent, 0.7f) : g_theme.divider);
    float pad = 16, x0 = area.left + pad, x1 = area.right - pad;
    float y = area.top + pad - g_pv.scroll;
    g_rt->PushAxisAlignedClip(R(area.left + 1, area.top, area.right, area.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    if (g_pv.path.empty()) {
        g_cv.Text(L"Select a file to preview it", g_fCenter, R(x0, area.top + 60, x1, area.top + 90), g_theme.subtext);
        g_rt->PopAxisAlignedClip();
        if (PreviewHandlerActive()) PreviewHandlerClose();
        return;
    }
    const PreviewResult* r = g_pv.res;
    if (g_pv.reloadBitmap && r) { g_pv.reloadBitmap = false; g_pv.bmp = MakeBitmap(r->img); }
    // ---- visual
    float visH = std::min((x1 - x0) * 0.85f, (area.bottom - area.top) * 0.55f);
    D2D1_RECT_F vis = R(x0, y, x1, y + visH);
    bool handler = r && r->kind == PreviewResult::Handler;
    if (handler) {
        vis.bottom = y + (area.bottom - area.top) * 0.62f;
        g_cv.FillRound(vis, 8, g_theme.surface);
        g_pv.contentRc = vis;
        if (g_pv.handlerShown) {
            RECT rc{(LONG)(vis.left * g_scale), (LONG)(vis.top * g_scale), (LONG)(vis.right * g_scale), (LONG)(vis.bottom * g_scale)};
            PreviewHandlerMove(rc);
        } else {
            g_cv.Text(L"Loading preview…", g_fCenter, vis, g_theme.subtext);
        }
        y = vis.bottom + 14;
    } else if (g_pv.bmp) {
        D2D1_SIZE_F bs = g_pv.bmp->GetSize();
        float k = std::min((vis.right - vis.left) / bs.width, (vis.bottom - vis.top) / bs.height);
        k = std::min(k, 1.0f);
        float w = bs.width * k, h = bs.height * k;
        float cx = (vis.left + vis.right) / 2;
        D2D1_RECT_F img = R(cx - w / 2, vis.top, cx + w / 2, vis.top + h);
        g_rt->DrawBitmap(g_pv.bmp, img, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        y = img.bottom + 14;
    } else if (r && r->kind == PreviewResult::Text) {
        // code / text: first part of the file in a monospace box
        float boxH = std::max(160.0f, (area.bottom - area.top) * 0.6f);
        D2D1_RECT_F box = R(x0, y, x1, y + boxH);
        g_cv.FillRound(box, 8, g_theme.surface);
        if (!g_fMono) g_fMono = Format(MonoFont(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
        g_rt->PushAxisAlignedClip(R(box.left + 2, box.top + 2, box.right - 2, box.bottom - 2), D2D1_ANTIALIAS_MODE_ALIASED);
        std::wstring shown = r->text.substr(0, 12000);
        g_cv.br->SetColor(g_theme.text);
        if (g_fMono) {
            IDWriteTextLayout* lay = nullptr;
            dw->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), g_fMono, box.right - box.left - 20, 100000, &lay);
            if (lay) {
                lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                g_rt->DrawTextLayout(D2D1::Point2F(box.left + 10, box.top + 8), lay, g_cv.br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                lay->Release();
            }
        }
        g_rt->PopAxisAlignedClip();
        y = box.bottom + 14;
    } else {
        // big icon
        float isz = 96;
        int icon = -1;
        Pane& t = T();
        if (t.focus >= 0 && t.focus < (int)t.view.size()) icon = t.items[t.view[t.focus]].icon;
        if (g_pv.isDir && _wcsicmp(g_pv.path.c_str(), t.path.c_str()) == 0) icon = FolderIconIndex();
        ID2D1Bitmap* tb = ThumbFor(g_pv.path, (int)(isz * g_scale));
        float cx = (x0 + x1) / 2;
        D2D1_RECT_F ir = R(cx - isz / 2, y + 10, cx + isz / 2, y + 10 + isz);
        if (tb) g_rt->DrawBitmap(tb, ir, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else DrawItemIcon(icon, ir.left, (ir.top + ir.bottom) / 2, isz);
        if (g_pv.loading) g_cv.Text(L"Loading…", g_fCenter, R(x0, ir.bottom + 4, x1, ir.bottom + 24), g_theme.subtext);
        y = ir.bottom + 30;
    }
    // ---- name + details
    g_cv.Text(PathLeaf(g_pv.path), g_fEmpty, R(x0, y, x1, y + 26), g_theme.text);
    y += 32;
    auto row = [&](const std::wstring& k, const std::wstring& v) {
        if (v.empty()) return;
        g_cv.Text(k, g_fSmall, R(x0, y, x0 + 92, y + 22), g_theme.subtext);
        g_cv.Text(v, g_fSmall, R(x0 + 96, y, x1, y + 22), g_theme.text);
        y += 22;
    };
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool haveAttrs = GetFileAttributesExW(g_pv.path.c_str(), GetFileExInfoStandard, &fa) != FALSE;
    FolderSize fs;
    bool sized = g_pv.isDir && SizesGet(g_pv.path, fs) && fs.done;
    if (g_pv.isDir) {
        if (sized) row(L"Size", FormatSize(fs.bytes) + L"  (" + WithCommas(fs.files) + L" files, " + WithCommas(fs.folders) + L" folders)");
        else if (SizesAllowed(g_pv.path)) row(L"Size", L"Calculating…");
        if (r && r->kind == PreviewResult::Folder) row(L"Contains", WithCommas(r->files) + L" files, " + WithCommas(r->folders) + L" folders");
    } else if (haveAttrs) {
        row(L"Size", FormatSize(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) + L"  (" +
                         WithCommas(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) + L" bytes)");
    }
    if (r && r->imgW) row(L"Dimensions", std::to_wstring(r->imgW) + L" × " + std::to_wstring(r->imgH));
    if (haveAttrs) {
        row(L"Modified", FormatDate(fa.ftLastWriteTime));
        row(L"Created", FormatDate(fa.ftCreationTime));
    }
    std::wstring tg = TagOf(g_pv.path);
    if (const TagDef* td = TagDefById(tg)) row(L"Tag", td->name);
    row(L"Location", PathParent(g_pv.path).empty() ? g_pv.path : PathParent(g_pv.path));
    if (r && r->kind == PreviewResult::Text && r->truncated) row(L"Preview", L"First part of the file");
    // ---- git
    if (g_pv.isDir && g_pv.gitLoaded && g_pv.git.isRepo) {
        y += 8;
        g_cv.Text(L"GIT", g_fSection, R(x0, y, x1, y + 22), g_theme.subtext);
        y += 24;
        row(L"Branch", g_pv.git.branch);
        if (g_pv.git.changed < 0) row(L"Status", L"Install Git to see changes");
        else if (g_pv.git.changed == 0 && g_pv.git.untracked == 0) row(L"Status", L"Clean");
        else row(L"Status", std::to_wstring(g_pv.git.changed) + L" changed" + (g_pv.git.untracked ? L", " + std::to_wstring(g_pv.git.untracked) + L" untracked" : L""));
        if (g_pv.git.ahead || g_pv.git.behind) row(L"Remote", std::to_wstring(g_pv.git.ahead) + L" ahead, " + std::to_wstring(g_pv.git.behind) + L" behind");
        if (_wcsicmp(g_pv.git.root.c_str(), g_pv.path.c_str()) != 0) row(L"Repo", g_pv.git.root);
    }
    // ---- what's taking space
    if (g_pv.isDir && SizesAllowed(g_pv.path)) {
        std::vector<std::pair<uint64_t, std::wstring>> subs;
        bool pending = false;
        for (auto& n : ListSubfolders(g_pv.path, 300, true)) {
            FolderSize s2;
            if (SizesGet(PathJoin(g_pv.path, n), s2) && s2.done) subs.push_back({s2.bytes, n});
            else pending = true;
        }
        std::sort(subs.begin(), subs.end(), [](auto& a, auto& b) { return a.first > b.first; });
        if (!subs.empty() || pending) {
            y += 8;
            g_cv.Text(L"WHAT'S TAKING SPACE", g_fSection, R(x0, y, x1, y + 22), g_theme.subtext);
            y += 26;
            uint64_t top = subs.empty() ? 1 : std::max<uint64_t>(1, subs[0].first);
            for (size_t i = 0; i < subs.size() && i < 8; ++i) {
                g_cv.Text(subs[i].second, g_fSmall, R(x0, y, x1 - 70, y + 18), g_theme.text);
                g_cv.Text(FormatSize(subs[i].first), g_fSmallRight, R(x1 - 80, y, x1, y + 18), g_theme.subtext);
                D2D1_RECT_F bar = R(x0, y + 19, x1, y + 23);
                g_cv.FillRound(bar, 2, g_theme.surfaceStrong);
                g_cv.FillRound(R(bar.left, bar.top, bar.left + (bar.right - bar.left) * std::max(0.01f, (float)((double)subs[i].first / top)), bar.bottom), 2,
                               g_theme.accent);
                y += 30;
            }
            if (pending) { g_cv.Text(L"Measuring…", g_fSmall, R(x0, y, x1, y + 18), g_theme.subtext); y += 22; }
        }
    }
    // ---- buttons
    y += 12;
    const wchar_t* labels[] = {L"Open", L"Open in other pane"};
    float bx = x0;
    for (int i = 0; i < 2; ++i) {
        float w = MeasureWidth(labels[i], g_fCenter) + 28;
        D2D1_RECT_F b = R(bx, y, bx + w, y + 32);
        AddZone(ZPreviewButton, i, b);
        bool hot = Hot(ZPreviewButton, i);
        if (i == 0) g_cv.FillRound(b, 16, hot ? Mix(g_theme.accent, D2D1::ColorF(1, 1, 1, 1), 0.12f) : g_theme.accent);
        else { g_cv.FillRound(b, 16, hot ? g_theme.surfaceStrong : g_theme.surface); g_cv.StrokeRound(b, 16, g_theme.divider); }
        g_cv.Text(labels[i], g_fCenter, b, i == 0 ? g_theme.onAccent : g_theme.text);
        bx += w + 8;
    }
    y += 44;
    g_rt->PopAxisAlignedClip();
    float contentH = y + g_pv.scroll - area.top;
    float maxS = std::max(0.0f, contentH - (area.bottom - area.top));
    if (g_pv.scroll > maxS) g_pv.scroll = maxS;
}

// ============================================================ toggles, updates, saved searches
static void ToggleDual() {
    g_config.layout = DualMode() ? L"single" : L"dual";
    ConfigSave();
    WatchActive();
    Redraw();
}

static void ToggleGrid() {
    Pane& t = T();
    t.viewMode = t.viewMode == ViewGrid ? ViewList : ViewGrid;
    SaveFolderView(t);
    EnsureVisible(t, t.focus);
    Redraw();
}

static void OpenSettings(int page) { SettingsOpen(page); }

static void RunSavedSearch(int idx, bool newTab) {
    if (idx < 0 || idx >= (int)g_config.savedSearches.size()) return;
    SavedSearch ss = g_config.savedSearches[idx];
    if (newTab) AddTab(ss.root, true, g_active + 1);
    Pane& t = T();
    if (_wcsicmp(t.path.c_str(), ss.root.c_str()) != 0) Navigate(t, ss.root);
    t.filter.SetText(ss.query);
    StartSearch(t, ss.query);
}

static void SaveCurrentSearch() {
    Pane& t = T();
    if (!t.searchActive) return;
    std::wstring name = t.searchQuery;
    if (!PromptText(g_hwnd, L"Save search", L"Name for this saved search (it appears in the sidebar):", name) || name.empty()) return;
    g_config.savedSearches.push_back({name, t.path, t.searchQuery});
    ConfigSave();
    SetStatus(L"Saved \"" + name + L"\" to the sidebar");
}

static void OnUpdateStatus() {
    updater::Status st = updater::Get();
    if (g_manualUpdateCheck) {
        if (st.state == updater::State::UpToDate) { SetStatus(L"Strata " STRATA_VERSION L" is up to date"); g_manualUpdateCheck = false; }
        else if (st.state == updater::State::Error) { SetStatus(L"Update check failed: " + st.message); g_manualUpdateCheck = false; }
        else if (st.state == updater::State::Available) g_manualUpdateCheck = false;
    }
    if (st.state == updater::State::Ready && g_installWhenReady) { g_installWhenReady = false; AutoUpdateInstallNow(); }
    SettingsOnUpdateStatus();
    Redraw();
}

static std::wstring PlainNotes(const std::wstring& md) {
    std::wstring out;
    size_t i = 0;
    while (i < md.size()) {
        size_t nl = md.find(L'\n', i);
        std::wstring line = md.substr(i, nl == std::wstring::npos ? std::wstring::npos : nl - i);
        i = nl == std::wstring::npos ? md.size() : nl + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t k = 0;
        while (k < line.size() && line[k] == L'#') ++k;
        if (k) line = line.substr(std::min(line.size(), k + 1));
        if (line.size() > 1 && (line[0] == L'-' || line[0] == L'*') && line[1] == L' ') line = L"\x2022 " + line.substr(2);
        std::wstring clean;
        for (wchar_t c : line) if (c != L'*' && c != L'`') clean += c;
        out += clean + L"\n";
    }
    return out;
}

static void UpdatePillClicked() {
    updater::Status st = updater::Get();
    if (st.state == updater::State::Ready) { AutoUpdateInstallNow(); return; }
    if (st.state != updater::State::Available) return;
    // changelog + one-click update
    std::wstring notes = PlainNotes(st.release.notes);
    if (notes.size() > 1800) notes = notes.substr(0, 1800) + L"…";
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = g_hwnd;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_COMMAND_LINKS;
    tc.pszWindowTitle = L"Strata update";
    std::wstring main = L"Strata " + st.release.version + L" is available";
    std::wstring content = L"You have " STRATA_VERSION L".\n\n" + notes;
    tc.pszMainInstruction = main.c_str();
    tc.pszContent = content.c_str();
    tc.pszMainIcon = TD_INFORMATION_ICON;
    TASKDIALOG_BUTTON buttons[] = {{100, L"Update now\nDownloads, verifies and restarts Strata. Your tabs come back."},
                                   {101, L"Later"}, {102, L"View on GitHub"}};
    tc.cButtons = 3;
    tc.pButtons = buttons;
    int b = 0;
    if (FAILED(TaskDialogIndirect(&tc, &b, nullptr, nullptr))) return;
    if (b == 100) {
        if (QueueBusy()) SetStatus(L"The update will install when your file transfers finish");
        g_installWhenReady = true;
        AutoUpdateDownloadNow();
    } else if (b == 102) {
        ShellExecuteW(nullptr, L"open", st.release.page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
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
    g_config.session.clear();
    for (auto& tb : g_tabs) {
        SessionTab st = TabState(tb);
        // search results / tag views come back as the folder they were in
        g_config.session.push_back(st);
    }
    g_config.sessionActive = g_active;
    ConfigSave();
}

// Everything that must happen before the updater swaps the exe and restarts Strata
static void ReleaseForUpdate() {
    if (g_focus == Focus::Rename) CommitRename(true);
    SaveSession();
    SettingsClose();
    PreviewHandlerClose();
    TrayRemove();
    QueueShutdown();
    ReleaseInstanceMutex();
}

void AppSettingsChanged() {
    CreateFormats();
    ApplyTheme();
    IndexApplySettings();
    for (auto& tb_ : g_tabs) for (auto& pn : tb_.pane) if (pn.loaded) BuildView(pn);
    WatchActive();
    if (g_config.keepInTray || StartWithWindowsEnabled()) TrayAdd(g_hwnd); else TrayRemove();
    PreviewRefresh(true);
    Redraw();
}

HWND AppWindow() { return g_hwnd; }

static void ShowMainWindow() {
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, g_config.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

void TrayOnMessage(HWND owner, LPARAM lp) {
    UINT ev = LOWORD(lp);
    if (ev == WM_LBUTTONUP || ev == NIN_SELECT || ev == NIN_KEYSELECT) { ShowMainWindow(); return; }
    if (ev != WM_CONTEXTMENU && ev != WM_RBUTTONUP) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open Strata");
    AppendMenuW(m, MF_STRING, 2, L"New tab");
    HMENU pins = CreatePopupMenu();
    for (int i = 0; i < (int)g_config.pins.size() && i < 30; ++i) {
        std::wstring n = g_config.pins[i].name.empty() ? PathLeaf(g_config.pins[i].path) : g_config.pins[i].name;
        AppendMenuW(pins, MF_STRING, 100 + i, n.c_str());
    }
    AppendMenuW(m, MF_POPUP | (g_config.pins.empty() ? MF_GRAYED : 0), (UINT_PTR)pins, L"Pinned folders");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Settings");
    AppendMenuW(m, MF_STRING, 4, L"Quit Strata");
    SetMenuDefaultItem(m, 1, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
    DestroyMenu(m);
    if (cmd == 1) ShowMainWindow();
    else if (cmd == 2) { ShowMainWindow(); AddTab(L"", true, g_active + 1); }
    else if (cmd == 3) SettingsOpen(-1);
    else if (cmd == 4) PostMessageW(g_hwnd, WM_CLOSE, 1, 0);
    else if (cmd >= 100 && cmd < 100 + g_config.pins.size()) { ShowMainWindow(); AddTab(g_config.pins[cmd - 100].path, true, g_active + 1); }
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
            if (wp == TIMER_UPDATE_CHECK) { AutoUpdateOnTimer(); return 0; }
            if (wp == TIMER_HEALTHY) { KillTimer(hwnd, TIMER_HEALTHY); AutoUpdateOnStarted(); return 0; }
            switch (wp) {
                case TIMER_PREVIEW: KillTimer(hwnd, TIMER_PREVIEW); ShowHandlerNow(); return 0;
                case TIMER_QUEUECLOSE:
                    KillTimer(hwnd, TIMER_QUEUECLOSE);
                    if (!QueueBusy() && !g_queueTouched) { g_queueOpen = false; QueueClearFinished(); Redraw(); }
                    return 0;
                case TIMER_SCROLL: {
                    bool moving = false;
                    float k = g_config.animation == L"none" ? 1.0f : g_config.animation == L"fast" ? 0.5f : 0.32f;
                    for (auto& pn : TB().pane) {
                        float d = pn.scrollTarget - pn.scroll;
                        if (std::fabs(d) < 0.5f) pn.scroll = pn.scrollTarget;
                        else { pn.scroll += d * k; moving = true; }
                    }
                    if (!moving) KillTimer(hwnd, TIMER_SCROLL);
                    if (g_hwnd) {
                        POINT pt;
                        GetCursorPos(&pt);
                        ScreenToClient(hwnd, &pt);
                        UpdateHover(Px2Dip(pt.x), Px2Dip(pt.y));
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
                case TIMER_REFRESH1: {
                    KillTimer(hwnd, wp);
                    if (g_tabs.empty()) return 0;
                    Pane& p = TB().pane[wp == TIMER_REFRESH1 ? 1 : 0];
                    if (!(g_focus == Focus::Rename && &p == &T()) && !p.searchActive && p.tagFilter.empty()) Load(p, true);
                    return 0;
                }
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
                    Pane& t = T();
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
        case WM_STRATA_JOBDONE: OnJobDone(reinterpret_cast<JobResult*>(lp)); return 0;
        case WM_STRATA_QUEUEASK: OnQueueAsk(reinterpret_cast<QueueAsk*>(lp)); return 0;
        case WM_STRATA_QUEUE: Redraw(); return 0;
        case WM_STRATA_SIZES:
            for (auto& tb_ : g_tabs) for (auto& pn : tb_.pane) if (pn.loaded && !DriveView(pn)) BuildView(pn);
            Redraw();
            return 0;
        case WM_STRATA_UPDATE: AutoUpdateOnStatus(); OnUpdateStatus(); return 0;
        case WM_STRATA_THUMB: OnThumb(reinterpret_cast<ThumbResult*>(lp)); return 0;
        case WM_STRATA_PREVIEW: OnPreview(reinterpret_cast<PreviewResult*>(lp)); return 0;
        case WM_STRATA_GIT: OnGit(reinterpret_cast<GitResult*>(lp)); return 0;
        case WM_STRATA_INDEX: SettingsOnUpdateStatus(); return 0;
        case WM_STRATA_TRAY: TrayOnMessage(hwnd, lp); return 0;
        case WM_STRATA_SEARCH: OnSearch(reinterpret_cast<SearchBatch*>(lp)); return 0;
        case WM_STRATA_DIRCHANGED:
            if ((uint64_t)wp == WatchGeneration((int)lp)) SetTimer(hwnd, lp ? TIMER_REFRESH1 : TIMER_REFRESH, 350, nullptr);
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
                ShowMainWindow();
                if (!path.empty()) MainOpenPath(path, true);
            }
            return TRUE;
        }
        case WM_CLOSE:
            if (g_focus == Focus::Rename) CommitRename(true);
            SaveSession();
            if (wp != 1 && g_config.keepInTray) {
                // stay ready in the tray for instant opening
                ShowWindow(hwnd, SW_HIDE);
                PreviewHandlerClose();
                g_pv.handlerShown = false;
                SettingsClose();
                return 0;
            }
            if (QueueBusy() && MessageBoxW(hwnd, L"A file transfer is still running. Quit and cancel it?", L"Strata",
                                           MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return 0;
            QueueShutdown();
            SettingsClose();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            WatchFolder(hwnd, 0, L"");
            WatchFolder(hwnd, 1, L"");
            PreviewClear();
            TrayRemove();
            DragDropRevoke(hwnd);
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
            Pane& pn = g_tabs[i].pane[g_tabs[i].active];
            if (!newTab) Navigate(pn, parent);
            pn.selectAfterLoad = PathLeaf(p);
            return;
        }
        p.clear();
    }
    if (newTab) AddTab(p, true, (int)g_tabs.size());
    else Navigate(T(), p);
}

HWND MainCreate(HINSTANCE inst, const std::vector<std::wstring>& openPaths, bool startHidden, const wchar_t* cmdLine) {
    if (!gfx::Init()) { Log(L"Direct2D init failed"); return nullptr; }
    TagsLoad();
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

    // background engines
    QueueInit(g_hwnd);
    SizesInit(g_hwnd);
    IndexInit(g_hwnd);
    AutoUpdateInit(g_hwnd, cmdLine);
    g_releaseAppForUpdate = ReleaseForUpdate;
    AutoUpdateApplySettings();
    DefaultFileManagerRepairPath();

    // drag and drop into Strata: onto a folder row, a pane, a tab, a pin or a drive
    DropHandlers dh;
    dh.targetAt = [](POINT cp) -> std::wstring {
        float x = Px2Dip(cp.x), y = Px2Dip(cp.y);
        Zone z = HitZoneTop(x, y);
        switch (z.kind) {
            case ZList: {
                Pane& pn = PaneAt(z.pane);
                if (DriveView(pn) || !pn.archive.empty()) {
                    int v = ItemAt(pn, x, y);
                    return (DriveView(pn) && v >= 0) ? pn.items[pn.view[v]].path : L"";
                }
                int v = ItemAt(pn, x, y);
                if (v >= 0 && pn.items[pn.view[v]].isDir()) return ItemPath(pn, pn.items[pn.view[v]]);
                return (pn.searchActive || !pn.tagFilter.empty()) ? L"" : pn.path;
            }
            case ZTab: { const Tab& tb = g_tabs[z.idx]; return tb.pane[tb.active].path; }
            case ZSidePin: return g_config.pins[z.idx].path;
            case ZSideDrive: return g_drives[z.idx].root;
            case ZSideRecent: return g_config.recentFolders[z.idx];
            case ZCrumb: return g_crumbs[z.idx].path;
            default: return L"";
        }
    };
    dh.hover = [](const std::wstring& folder) {
        if (folder != g_dropFolder) { g_dropFolder = folder; Redraw(); }
    };
    dh.drop = [](const std::vector<std::wstring>& paths, const std::wstring& folder, DWORD effect) {
        g_dropFolder.clear();
        if (effect & DROPEFFECT_LINK) {
            for (auto& p : paths) CreateShortcutIn(p, folder);
            ReloadPanesFor({PathJoin(folder, L"x")});
        } else {
            StartTransfer((effect & DROPEFFECT_MOVE) ? JobKind::Move : JobKind::Copy, paths, folder);
        }
        SetForegroundWindow(g_hwnd);
    };
    DragDropRegister(g_hwnd, dh);

    // tabs: restored session, then anything passed on the command line
    if (g_config.restoreSession)
        for (auto& st : g_config.session) {
            std::wstring l = st.left.empty() || IsDirectory(st.left) || !ArchiveOf(st.left).empty() ? st.left : L"";
            std::wstring r2 = st.right.empty() || IsDirectory(st.right) || !ArchiveOf(st.right).empty() ? st.right : L"";
            AddTab(l, false, -1, &r2, st.activePane);
        }
    int active = std::clamp(g_config.sessionActive, 0, std::max(0, (int)g_tabs.size() - 1));
    for (auto& p : openPaths) { MainOpenPath(p, true); active = (int)g_tabs.size() - 1; }
    if (g_tabs.empty()) {
        // first run: This PC on the left, your profile folder on the right
        PWSTR prof = nullptr;
        std::wstring right;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &prof))) right = prof;
        CoTaskMemFree(prof);
        AddTab(L"", false, -1, &right, 0);
    }
    g_active = std::clamp(active, 0, (int)g_tabs.size() - 1);
    WatchActive();
    ListDrivesAsync(g_hwnd);
    SetTimer(g_hwnd, TIMER_DRIVES, 60 * 1000, nullptr);
    if (g_config.keepInTray || StartWithWindowsEnabled()) TrayAdd(g_hwnd);

    if (!startHidden) {
        ShowWindow(g_hwnd, g_config.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        UpdateWindow(g_hwnd);
    }
    if (!CrashGuardMessage().empty()) SetStatus(CrashGuardMessage());
    if (!AutoUpdateFailedVersion().empty())
        SetStatus(L"The update to " + AutoUpdateFailedVersion() + L" didn't start, so Strata " STRATA_VERSION L" was restored");
    return g_hwnd;
}
