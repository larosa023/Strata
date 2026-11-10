// Strata - settings window (custom drawn, Mica, changes apply live). The drawing/input engine is
// shared with Foyer's settings so the suite looks the same.
#include "common.h"
#include "settings.h"
#include "dialogs.h"
#include "config.h"
#include "gfx.h"
#include "updater.h"
#include "autoupdate.h"
#include "system.h"
#include "tags.h"
#include "search.h"
#include "json.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include "index.h"
#include <functional>
#include <map>
#include <algorithm>
#include <cmath>
#include <ctime>

using namespace gfx;

// ============================================================ control model
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
enum class CType { Header, Toggle, Choice, Slider, Swatches, Buttons, Info, ThemeGrid, LayoutGrid, PinRow, HiddenRow, TagRow, Notes, UpdateCard, Hero };
}  // namespace

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Ctl {
    CType type;
    std::wstring label, desc;
    std::vector<std::wstring> options;
    std::function<int()> geti;
    std::function<void(int)> seti;
    std::function<bool()> getb;
    std::function<void(bool)> setb;
    std::function<float()> getf;
    std::function<void(float)> setf;
    float fmin = 0, fmax = 1;
    std::function<std::wstring(float)> fmt;
    std::vector<std::pair<std::wstring, std::function<void()>>> buttons;
    int primary = -1;              // index of the accent button
    int index = 0;                 // pin / tag index
    bool enabled = true;
};
}  // namespace

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Zone { int ctl; int part; D2D1_RECT_F rc; };
}  // namespace

static HWND g_hwnd;
static UINT g_dpi = 96;
static Theme g_theme;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_br;
static Canvas g_cv;
static IDWriteTextFormat *g_fTitle, *g_fBrand, *g_fNav, *g_fLabel, *g_fDesc, *g_fHeader, *g_fBtn, *g_fIcon, *g_fIconBig,
    *g_fSmallC, *g_fNotes, *g_fHero, *g_fCard;
static int g_page = PageGeneral;
static std::vector<Ctl> g_ctls;
static std::vector<Zone> g_zones;
static int g_hotCtl = -1, g_hotPart = -1;
static int g_dragCtl = -1;
static float g_scroll = 0, g_contentH = 0, g_viewH = 0;
static bool g_tracking = false;
static std::map<std::wstring, float> g_anim;   // toggle knob positions
static bool g_animTimer = false;
constexpr UINT_PTR TIMER_SANIM = 1;

static const float kSidebarW = 240;
static const wchar_t* kPageNames[PageCount] = {L"General", L"Layout", L"Themes", L"Search", L"Tags", L"File operations", L"Updates", L"About"};
static const wchar_t kPageGlyphs[PageCount] = {0xE80F, 0xF0E2, 0xE790, 0xE721, 0xE8EC, 0xE8C8, 0xE895, 0xE946};

static float Dip(int px) { return px * 96.0f / g_dpi; }
static int Px(float d) { return (int)std::lround(d * g_dpi / 96.0f); }
static bool InR(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static void ShellOpen(const wchar_t* what) { ShellExecuteW(nullptr, L"open", what, nullptr, nullptr, SW_SHOWNORMAL); }

bool SettingsIsOpen() { return g_hwnd && IsWindow(g_hwnd); }
void SettingsClose() { if (SettingsIsOpen()) DestroyWindow(g_hwnd); }

// ============================================================ applying changes
static void ApplyTheme();
static void BuildPage();

static void Changed(bool save = true) {
    if (save) ConfigSave();
    AutoUpdateApplySettings();
    AppSettingsChanged();
    ApplyTheme();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static std::wstring PickFile(bool save, const wchar_t* defName, const wchar_t* filter, const wchar_t* ext) {
    wchar_t path[MAX_PATH] = {};
    if (save) wcsncpy_s(path, defName, _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = ext;
    ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? std::wstring(path) : L"";
}

// ============================================================ text prompt
bool PromptText(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value) {
    return TextDialog(owner, title, prompt, value, L"OK");
}

// ============================================================ pages
static Ctl Header(const wchar_t* t) { Ctl c{CType::Header}; c.label = t; return c; }

static Ctl Toggle(const std::wstring& label, const wchar_t* desc, std::function<bool()> get, std::function<void(bool)> set) {
    Ctl c{CType::Toggle}; c.label = label; c.desc = desc ? desc : L""; c.getb = get; c.setb = set; return c;
}

static Ctl Choice(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts, std::function<int()> get, std::function<void(int)> set) {
    Ctl c{CType::Choice}; c.label = label; c.desc = desc ? desc : L""; c.options = std::move(opts); c.geti = get; c.seti = set; return c;
}

static Ctl Buttons(const wchar_t* label, const wchar_t* desc, std::vector<std::pair<std::wstring, std::function<void()>>> b, int primary = -1) {
    Ctl c{CType::Buttons}; c.label = label ? label : L""; c.desc = desc ? desc : L""; c.buttons = std::move(b); c.primary = primary; return c;
}

static Ctl Info(const std::wstring& text) { Ctl c{CType::Info}; c.desc = text; return c; }

static Ctl Slider(const wchar_t* label, const wchar_t* desc, float lo, float hi, std::function<float()> get, std::function<void(float)> set,
                  std::function<std::wstring(float)> fmt) {
    Ctl c{CType::Slider}; c.label = label; c.desc = desc ? desc : L""; c.fmin = lo; c.fmax = hi; c.getf = get; c.setf = set; c.fmt = fmt; return c;
}

static int IndexOf(const std::wstring& v, std::initializer_list<const wchar_t*> list) {
    int i = 0;
    for (auto s : list) { if (v == s) return i; ++i; }
    return 0;
}

static void BuildGeneral(std::vector<Ctl>& v) {
    v.push_back(Header(L"Startup"));
    v.push_back(Toggle(L"Start with Windows", L"Strata starts quietly in the tray when you sign in, so it opens instantly.",
                       [] { return StartWithWindowsEnabled(); }, [](bool b) { SetStartWithWindows(b); if (b) g_config.keepInTray = true; Changed(); }));
    v.push_back(Toggle(L"Keep running in the tray", L"Closing the window keeps Strata ready in the tray for instant opening.",
                       [] { return g_config.keepInTray; }, [](bool b) { g_config.keepInTray = b; Changed(); }));
    v.push_back(Toggle(L"Reopen my tabs", L"Restore the tabs and panes you had open last time.",
                       [] { return g_config.restoreSession; }, [](bool b) { g_config.restoreSession = b; Changed(); }));
    v.push_back(Header(L"Default file manager"));
    v.push_back(Toggle(L"Open folders with Strata", L"Win+E, double-clicking folders and \"Open\" on folders use Strata. File Explorer still works, "
                       L"and if Strata ever fails to open a folder, Explorer opens it instead.",
                       [] { return DefaultFileManagerEnabled(); },
                       [](bool b) {
                           std::wstring err;
                           if (!SetDefaultFileManager(b, &err)) NoticeDialog(g_hwnd, L"Something went wrong", err, true);
                           Changed(false);
                       }));
    if (!CrashGuardMessage().empty()) v.push_back(Info(CrashGuardMessage()));
    v.push_back(Header(L"Browsing"));
    v.push_back(Toggle(L"Show hidden items", L"Ctrl+H toggles this from anywhere.", [] { return g_config.showHidden; },
                       [](bool b) { g_config.showHidden = b; Changed(); }));
    v.push_back(Toggle(L"Show file extensions", nullptr, [] { return g_config.showExtensions; }, [](bool b) { g_config.showExtensions = b; Changed(); }));
    v.push_back(Toggle(L"Folders first", L"Keep folders above files whatever the sort order.", [] { return g_config.foldersFirst; },
                       [](bool b) { g_config.foldersFirst = b; Changed(); }));
    v.push_back(Toggle(L"Remember each folder's view", L"Sort order and list / grid view are remembered per folder.",
                       [] { return g_config.rememberFolderViews; }, [](bool b) { g_config.rememberFolderViews = b; Changed(); }));
    v.push_back(Header(L"Right-click menu"));
    v.push_back(Info(L"Turn off entries you never use. These are the Windows and app entries Strata has seen in your right-click menus. "
                     L"Shift+right-click always shows the full menu."));
    if (g_config.seenMenuItems.empty()) v.push_back(Info(L"Right-click a few files first and the entries will show up here."));
    std::vector<std::wstring> items = g_config.seenMenuItems;
    std::sort(items.begin(), items.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (auto& it : items) {
        std::wstring label = it;
        v.push_back(Toggle(label, nullptr,
                           [label] {
                               for (auto& h : g_config.hiddenMenuItems) if (_wcsicmp(h.c_str(), label.c_str()) == 0) return false;
                               return true;
                           },
                           [label](bool show) {
                               auto& hid = g_config.hiddenMenuItems;
                               hid.erase(std::remove_if(hid.begin(), hid.end(), [&](const std::wstring& h) { return _wcsicmp(h.c_str(), label.c_str()) == 0; }), hid.end());
                               if (!show) hid.push_back(label);
                               Changed();
                           }));
    }
}

static void BuildLayout(std::vector<Ctl>& v) {
    v.push_back(Header(L"Panes"));
    v.push_back(Ctl{CType::LayoutGrid});
    v.push_back(Header(L"Show"));
    v.push_back(Toggle(L"Sidebar", L"Ctrl+B", [] { return g_config.showSidebar; }, [](bool b) { g_config.showSidebar = b; Changed(); }));
    v.push_back(Toggle(L"Stack file versions", L"Plans_rev1, Plans_rev2 and Plans rev 3 show as one row with the newest on top. Click the "
                       L"\"versions\" pill (or press →) to see the older ones.",
                       [] { return g_config.stackVersions; }, [](bool b) { g_config.stackVersions = b; Changed(); }));
    v.push_back(Toggle(L"Preview panel", L"Ctrl+P. Space opens a big quick look you can flip through with the arrow keys.", [] { return g_config.showPreview; },
                       [](bool b) { g_config.showPreview = b; Changed(); }));
    v.push_back(Toggle(L"Status bar", nullptr, [] { return g_config.showStatusBar; }, [](bool b) { g_config.showStatusBar = b; Changed(); }));
    v.push_back(Toggle(L"Back / forward / up buttons", nullptr, [] { return g_config.showNavButtons; }, [](bool b) { g_config.showNavButtons = b; Changed(); }));
    v.push_back(Toggle(L"View switch", L"The dual / single / grid buttons in the toolbar.", [] { return g_config.showViewSwitch; },
                       [](bool b) { g_config.showViewSwitch = b; Changed(); }));
    v.push_back(Header(L"Sidebar sections"));
    v.push_back(Toggle(L"Tags", nullptr, [] { return g_config.sidebarTags; }, [](bool b) { g_config.sidebarTags = b; Changed(); }));
    v.push_back(Toggle(L"Saved searches", nullptr, [] { return g_config.sidebarSaved; }, [](bool b) { g_config.sidebarSaved = b; Changed(); }));
    v.push_back(Toggle(L"Recent folders", nullptr, [] { return g_config.sidebarRecent; }, [](bool b) { g_config.sidebarRecent = b; Changed(); }));
    v.push_back(Header(L"Size"));
    v.push_back(Choice(L"Row height", nullptr, {L"Compact", L"Comfortable"},
                       [] { return IndexOf(g_config.density, {L"compact", L"comfortable"}); },
                       [](int i) { g_config.density = i ? L"comfortable" : L"compact"; Changed(); }));
    v.push_back(Choice(L"Icon size", L"Also sets the tile size in grid view.", {L"Small", L"Medium", L"Large"},
                       [] { return IndexOf(g_config.iconSize, {L"small", L"medium", L"large"}); },
                       [](int i) { const wchar_t* o[] = {L"small", L"medium", L"large"}; g_config.iconSize = o[i]; Changed(); }));
    v.push_back(Slider(L"Text size", L"Strata also follows Windows text scaling (Settings > Accessibility > Text size).", 11, 16,
                       [] { return g_config.fontSize; }, [](float f) { g_config.fontSize = std::round(f * 2) / 2; Changed(false); },
                       [](float f) { wchar_t b[16]; swprintf(b, 16, L"%.1f pt", f); return std::wstring(b); }));
    v.push_back(Choice(L"Font", nullptr, {L"Segoe UI Variable", L"Segoe UI", L"Inter", L"Cascadia"},
                       [] { return IndexOf(g_config.fontFamily, {L"", L"Segoe UI", L"Inter", L"Cascadia Code"}); },
                       [](int i) { const wchar_t* o[] = {L"", L"Segoe UI", L"Inter", L"Cascadia Code"}; g_config.fontFamily = o[i]; Changed(); }));
    v.push_back(Header(L"Motion"));
    v.push_back(Choice(L"Animations", L"Smooth scrolling, hover fades and panel transitions. None is the fastest.", {L"None", L"Fast", L"Smooth"},
                       [] { return IndexOf(g_config.animation, {L"none", L"fast", L"smooth"}); },
                       [](int i) { const wchar_t* o[] = {L"none", L"fast", L"smooth"}; g_config.animation = o[i]; Changed(); }));
}

static std::wstring FoyerConfigPath() {
    PWSTR appData = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) r = std::wstring(appData) + L"\\Foyer\\config.json";
    CoTaskMemFree(appData);
    return r;
}

static void BuildThemes(std::vector<Ctl>& v) {
    v.push_back(Info(L"Changes show up in the Strata window right away."));
    v.push_back(Header(L"Theme"));
    v.push_back(Ctl{CType::ThemeGrid});
    if (g_config.theme == L"custom")
        v.push_back(Info(L"Using the imported theme \"" + g_config.customTheme + L"\". Pick a preset above to switch back."));
    v.push_back(Header(L"Color"));
    Ctl sw{CType::Swatches};
    sw.label = L"Accent color";
    sw.desc = L"The first swatch follows your Windows accent color.";
    v.push_back(sw);
    v.push_back(Header(L"Material"));
    bool supported = SystemBackdropSupported();
    Ctl bg = Choice(L"Background", supported ? L"Mica tints the window with your wallpaper; acrylic blurs what's behind it."
                                             : L"Mica and acrylic need Windows 11 version 22H2 or newer.",
                    {L"Solid", L"Mica", L"Acrylic"},
                    [] { return IndexOf(g_config.backdrop, {L"solid", L"mica", L"acrylic"}); },
                    [](int i) { const wchar_t* o[] = {L"solid", L"mica", L"acrylic"}; g_config.backdrop = o[i]; Changed(); });
    bg.enabled = supported;
    v.push_back(bg);
    Ctl tint = Slider(L"Tint", L"Lower values let more of the background show through.", 0.4f, 1.0f,
                      [] { return g_config.opacity; }, [](float f) { g_config.opacity = f; Changed(false); },
                      [](float f) { return std::to_wstring((int)std::lround(f * 100)) + L"%"; });
    tint.enabled = supported && g_config.backdrop != L"solid";
    v.push_back(tint);
    v.push_back(Slider(L"Corner radius", L"Roundness of rows, boxes and buttons.", 0, 16,
                       [] { return g_config.cornerRadius; }, [](float f) { g_config.cornerRadius = std::round(f); Changed(false); },
                       [](float f) { return std::to_wstring((int)std::lround(f)) + L" px"; }));
    v.push_back(Toggle(L"Follow Windows high contrast", L"Use the system's high-contrast colors when high contrast is on.",
                       [] { return g_config.respectHighContrast; }, [](bool b) { g_config.respectHighContrast = b; Changed(); }));
    v.push_back(Header(L"Theme files"));
    v.push_back(Buttons(L"Share themes", L"Themes are small .json files you can share. Foyer and Strata read the same format.",
                        {{L"Import\x2026", [] {
                              std::wstring p = PickFile(false, L"", L"Theme files (*.json)\0*.json\0All files\0*.*\0", L"json");
                              if (p.empty()) return;
                              std::wstring err;
                              if (ThemeImport(p, &err)) Changed(); else NoticeDialog(g_hwnd, L"Something went wrong", err, true);
                          }},
                         {L"Export\x2026", [] {
                              std::wstring p = PickFile(true, L"My theme.theme.json", L"Theme files (*.json)\0*.json\0", L"json");
                              if (p.empty()) return;
                              std::wstring name = PathLeaf(p);
                              size_t dot = name.find(L'.');
                              if (dot != std::wstring::npos) name = name.substr(0, dot);
                              NoticeDialog(g_hwnd, ThemeExport(p, name) ? L"Theme saved" : L"Couldn't save the theme", L"", false);
                          }}}));
    if (GetFileAttributesW(FoyerConfigPath().c_str()) != INVALID_FILE_ATTRIBUTES)
        v.push_back(Buttons(L"Match Foyer", L"Copy the theme, accent color and background from Foyer.",
                            {{L"Use Foyer's theme", [] {
                                  std::wstring err;
                                  if (ThemeImport(FoyerConfigPath(), &err)) Changed(); else NoticeDialog(g_hwnd, L"Something went wrong", err, true);
                              }}}, 0));
}

static void BuildSearch(std::vector<Ctl>& v) {
    v.push_back(Header(L"Whole-PC search"));
    bool ev = EverythingAvailable();
    v.push_back(Toggle(L"Use Everything when it's running",
                       ev ? L"Everything is running: searches from This PC are instant." : L"Everything (voidtools.com) isn't running right now.",
                       [] { return g_config.useEverything; }, [](bool b) { g_config.useEverything = b; Changed(); }));
    if (!ev) v.push_back(Buttons(L"Get Everything", L"A free, tiny search engine that indexes your drives in seconds.",
                                 {{L"Open voidtools.com", [] { ShellOpen(L"https://www.voidtools.com/"); }}}));
    v.push_back(Toggle(L"Strata's own index", L"Without Everything, Strata keeps its own list of every file on your fixed drives so search is instant. "
                       L"It builds in the background at low priority and uses about 100 MB of memory per million files.",
                       [] { return g_config.useIndex; }, [](bool b) { g_config.useIndex = b; Changed(); }));
    v.push_back(Info(IndexStatusText()));
    v.push_back(Toggle(L"Include network drives", L"Searching network drives is slow; leave this off unless you need it.",
                       [] { return g_config.searchNetwork; }, [](bool b) { g_config.searchNetwork = b; Changed(); }));
    v.push_back(Header(L"Search syntax"));
    v.push_back(Info(L"Words match anywhere in the name; all words must match. *.pdf and report?.docx use wildcards.\n"
                     L"Filters:  type:pdf   size:>50mb   size:<1kb   modified:7d   modified:2026   tag:urgent   folder: / file:\n"
                     L"In a folder, typing filters what's there; press Enter to search that folder and everything inside it."));
    v.push_back(Header(L"Saved searches"));
    if (g_config.savedSearches.empty()) v.push_back(Info(L"None yet. After searching, right-click the empty area of the results and choose Save search."));
    for (int i = 0; i < (int)g_config.savedSearches.size(); ++i) {
        const SavedSearch& s = g_config.savedSearches[i];
        Ctl c{CType::HiddenRow};
        c.label = s.name;
        c.desc = (s.root.empty() ? L"Everywhere" : s.root) + L"  \x00B7  " + s.query;
        c.index = i;
        v.push_back(c);
    }
}

static void BuildTags(std::vector<Ctl>& v) {
    v.push_back(Info(L"Right-click any file or folder \x2192 Tag to color-code it. Tagged items show a colored dot, and the sidebar lists everything "
                     L"with each tag. Tags stay with files you rename or move in Strata."));
    v.push_back(Header(L"Your tags"));
    for (int i = 0; i < (int)g_config.tags.size(); ++i) {
        Ctl c{CType::TagRow};
        c.label = g_config.tags[i].name;
        c.desc = std::to_wstring(TagsFind(g_config.tags[i].id).size()) + L" tagged";
        c.index = i;
        v.push_back(c);
    }
    v.push_back(Buttons(nullptr, nullptr, {{L"Add tag", [] {
        std::wstring name = L"New tag";
        if (!PromptText(g_hwnd, L"New tag", L"Tag name:", name) || name.empty()) return;
        static const uint32_t colors[] = {0xE5484D, 0xF5A524, 0xF2D13A, 0x30A46C, 0x3E8EF7, 0x8E4EC6, 0xE93D82, 0x12A594};
        TagDef t{L"t" + std::to_wstring((long long)time(nullptr)), name, colors[g_config.tags.size() % 8]};
        g_config.tags.push_back(t);
        Changed();
        BuildPage();
    }}, {L"Reset to defaults", [] {
        if (!ConfirmDialog(g_hwnd, L"Reset tags?", L"Your tags are replaced with Urgent, Active, Done and Repo. Items keep tags whose names still exist.",
                           L"Reset tags")) return;
        ConfigDefaultTags();
        Changed();
        BuildPage();
    }}}, 0));
}

static void BuildFileOps(std::vector<Ctl>& v) {
    v.push_back(Header(L"Copying and moving"));
    v.push_back(Toggle(L"Verify copies", L"After copying, compare each file with the original (SHA-256). Slower, but catches bad drives and cables.",
                       [] { return g_config.verifyCopies; }, [](bool b) { g_config.verifyCopies = b; Changed(); }));
    v.push_back(Choice(L"When a file already exists", nullptr, {L"Ask", L"Skip", L"Replace", L"Keep both"},
                       [] { return IndexOf(g_config.conflictDefault, {L"ask", L"skip", L"replace", L"keepboth"}); },
                       [](int i) { const wchar_t* o[] = {L"ask", L"skip", L"replace", L"keepboth"}; g_config.conflictDefault = o[i]; Changed(); }));
    v.push_back(Toggle(L"Play a sound when a long transfer finishes", L"A soft chime when a copy or move that took more than 10 seconds is done.",
                       [] { return g_config.soundOnFinish; }, [](bool b) { g_config.soundOnFinish = b; Changed(); }));
    v.push_back(Toggle(L"Show a notification when a long transfer finishes", L"A small card in the corner with \"Open folder\", or a Windows notification "
                       L"when Strata isn't in front.",
                       [] { return g_config.notifyOnFinish; }, [](bool b) { g_config.notifyOnFinish = b; Changed(); }));
    v.push_back(Toggle(L"Skip files that can't be copied", L"Keep going and list the problems at the end instead of asking each time.",
                       [] { return g_config.skipErrors; }, [](bool b) { g_config.skipErrors = b; Changed(); }));
    v.push_back(Header(L"Deleting"));
    v.push_back(Toggle(L"Ask before moving to the Recycle Bin", L"Deleting is undoable with Ctrl+Z either way. Shift+Delete always asks.",
                       [] { return g_config.confirmDelete; }, [](bool b) { g_config.confirmDelete = b; Changed(); }));
    v.push_back(Header(L"Folder sizes"));
    v.push_back(Toggle(L"Calculate folder sizes", L"Sizes are worked out in the background at low priority and pause while you're gaming.",
                       [] { return g_config.folderSizes; }, [](bool b) { g_config.folderSizes = b; Changed(); }));
    v.push_back(Toggle(L"On network drives too", L"Can be slow on big shares.", [] { return g_config.folderSizesNetwork; },
                       [](bool b) { g_config.folderSizesNetwork = b; Changed(); }));
}
static std::wstring CleanNotes(const std::wstring& md) {
    std::wstring out;
    size_t i = 0;
    while (i < md.size()) {
        size_t nl = md.find(L'\n', i);
        std::wstring line = md.substr(i, nl == std::wstring::npos ? std::wstring::npos : nl - i);
        i = nl == std::wstring::npos ? md.size() : nl + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t k = 0;
        while (k < line.size() && line[k] == L'#') ++k;
        if (k) line = line.substr(line.find_first_not_of(L' ', k) == std::wstring::npos ? line.size() : line.find_first_not_of(L' ', k));
        if (line.size() > 1 && (line[0] == L'-' || line[0] == L'*') && line[1] == L' ') line = L"\x2022 " + line.substr(2);
        std::wstring clean;
        for (wchar_t c : line) if (c != L'*' && c != L'`') clean += c;
        out += clean + L"\n";
    }
    while (!out.empty() && (out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    return out;
}

static void BuildUpdates(std::vector<Ctl>& v) {
    v.push_back(Ctl{CType::UpdateCard});
    v.push_back(Header(L"Automatic updates"));
    v.push_back(Toggle(L"Check for updates automatically", L"Strata quietly checks GitHub Releases in the background.",
                       [] { return g_config.autoCheck; }, [](bool b) { g_config.autoCheck = b; Changed(); }));
    v.push_back(Choice(L"Check every", nullptr, {L"Hour", L"6 hours", L"Day"},
                       [] { return g_config.checkHours <= 1 ? 0 : g_config.checkHours <= 6 ? 1 : 2; },
                       [](int i) { const int h[] = {1, 6, 24}; g_config.checkHours = h[i]; Changed(); }));
    v.push_back(Toggle(L"Install updates automatically", L"Downloads, verifies and installs in the background, then restarts with your tabs restored. Never during a file transfer, a game, fullscreen video or a presentation.",
                       [] { return g_config.autoInstall; }, [](bool b) { g_config.autoInstall = b; Changed(); }));
    v.push_back(Choice(L"Channel", L"Beta gets new features first and may be less polished.", {L"Stable", L"Beta"},
                       [] { return g_config.channel == L"beta" ? 1 : 0; },
                       [](int i) { g_config.channel = i ? L"beta" : L"stable"; Changed(); }));
    updater::Status st = updater::Get();
    if (!st.release.notes.empty()) {
        v.push_back(Header((L"What's new in " + st.release.version).c_str()));
        Ctl n{CType::Notes};
        n.desc = CleanNotes(st.release.notes);
        v.push_back(n);
    }
}

static void BuildAbout(std::vector<Ctl>& v) {
    v.push_back(Ctl{CType::Hero});
    v.push_back(Buttons(L"Project", L"Source code, releases and issue tracker on GitHub.",
                        {{L"Open GitHub", [] { ShellOpen(L"https://github.com/larosa023/Strata"); }},
                         {L"Release notes", [] { ShellOpen(L"https://github.com/larosa023/Strata/releases"); }}}));
    v.push_back(Header(L"Install"));
    if (IsInstalledCopy()) {
        v.push_back(Buttons(L"Installed", L"Strata is installed for your account (Start menu, Apps & features).",
                            {{L"Uninstall\x2026", [] {
                                  DlgSpec d;
                                  d.heading = L"Uninstall Strata?";
                                  d.body = L"Strata is removed from your programs folder, the Start menu and Apps & features.";
                                  d.icon = DlgSpec::Question;
                                  d.buttons = {{IDCANCEL, L"Cancel"}, {IDNO, L"Remove settings too", false, true}, {IDYES, L"Uninstall, keep settings", true}};
                                  d.cancelId = IDCANCEL;
                                  int r = ShowDialog(g_hwnd, d).id;
                                  if (r == IDCANCEL) return;
                                  UninstallStrata(r == IDYES);
                                  PostMessageW(AppWindow(), WM_CLOSE, 1, 0);
                                  PostQuitMessage(0);
                              }}}));
    } else {
        v.push_back(Buttons(L"Install Strata", L"Copies Strata to your programs folder and adds it to the Start menu and Apps & features. "
                            L"No admin rights needed.",
                            {{L"Install", [] {
                                  std::wstring err;
                                  if (!InstallStrata(&err)) { NoticeDialog(g_hwnd, L"Strata couldn't be installed", err, true); return; }
                                  if (ConfirmDialog(g_hwnd, L"Strata is installed", L"Switch to the installed copy now? Your tabs come back.", L"Switch now",
                                                    false, DlgSpec::Info)) {
                                      std::wstring exe = InstallDir() + L"\\Strata.exe";
                                      PostMessageW(AppWindow(), WM_CLOSE, 1, 0);
                                      ShellExecuteW(nullptr, L"open", exe.c_str(), L"--wait-old", nullptr, SW_SHOWNORMAL);
                                  }
                              }}}, 0));
    }
    v.push_back(Toggle(L"Portable mode", L"Keep settings in a \"data\" folder next to Strata.exe instead of your user profile, e.g. on a USB drive.",
                       [] { return IsPortable(); },
                       [](bool b) {
                           std::wstring err;
                           if (!SetPortable(b, &err)) NoticeDialog(g_hwnd, L"Something went wrong", err, true);
                           else NoticeDialog(g_hwnd, L"Done", L"Restart Strata to use the new settings location.");
                           BuildPage();
                       }));
    v.push_back(Header(L"Your data"));
    v.push_back(Buttons(L"Backup", L"Save all settings, pins, tags, saved searches, themes and folder views to one file.",
                        {{L"Back up\x2026", [] {
                              std::wstring p = PickFile(true, L"Strata-backup.json", L"Strata backup (*.json)\0*.json\0All files\0*.*\0", L"json");
                              if (p.empty()) return;
                              std::wstring err;
                              if (ConfigBackupTo(p, &err)) NoticeDialog(g_hwnd, L"Backup saved", L"");
                              else NoticeDialog(g_hwnd, L"Something went wrong", err, true);
                          }},
                         {L"Restore\x2026", [] {
                              std::wstring p = PickFile(false, L"", L"Strata backup (*.json)\0*.json\0All files\0*.*\0", L"json");
                              if (p.empty()) return;
                              std::wstring err;
                              if (ConfigRestoreFrom(p, &err)) { Changed(false); BuildPage(); NoticeDialog(g_hwnd, L"Settings restored", L""); }
                              else NoticeDialog(g_hwnd, L"Something went wrong", err, true);
                          }}}));
    v.push_back(Buttons(L"Files", L"Settings live in config.json; strata.log helps diagnose problems.",
                        {{L"Open data folder", [] { ShellOpen(DataDir().c_str()); }},
                         {L"Open log", [] { ShellOpen((DataDir() + L"\\strata.log").c_str()); }}}));
    v.push_back(Header(L"Keyboard"));
    v.push_back(Info(L"Tab switch pane  \x00B7  F5 copy to other pane  \x00B7  F6 move  \x00B7  F2 rename  \x00B7  Del Recycle Bin  \x00B7  Ctrl+Z undo\n"
                     L"Ctrl+K search  \x00B7  Ctrl+L path  \x00B7  Space preview  \x00B7  Ctrl+G grid  \x00B7  Ctrl+\\ dual pane  \x00B7  Ctrl+P preview panel\n"
                     L"Ctrl+T new tab  \x00B7  Ctrl+W close  \x00B7  Ctrl+Shift+T reopen  \x00B7  Alt+\x2190/\x2192 back/forward  \x00B7  Backspace up\n"
                     L"Ctrl+C / X / V  \x00B7  Ctrl+Shift+C copy path  \x00B7  Ctrl+Shift+N new folder  \x00B7  Ctrl+H hidden items  \x00B7  Ctrl+, settings"));
}

static void BuildPage() {
    g_ctls.clear();
    switch (g_page) {
        case PageGeneral: BuildGeneral(g_ctls); break;
        case PageLayout: BuildLayout(g_ctls); break;
        case PageThemes: BuildThemes(g_ctls); break;
        case PageSearch: BuildSearch(g_ctls); break;
        case PageTags: BuildTags(g_ctls); break;
        case PageFileOps: BuildFileOps(g_ctls); break;
        case PageUpdates: BuildUpdates(g_ctls); break;
        case PageAbout: BuildAbout(g_ctls); break;
    }
}

// ============================================================ resources
static void ReleaseFormats() {
    IDWriteTextFormat** all[] = {&g_fTitle, &g_fBrand, &g_fNav, &g_fLabel, &g_fDesc, &g_fHeader, &g_fBtn, &g_fIcon, &g_fIconBig, &g_fSmallC, &g_fNotes, &g_fHero, &g_fCard};
    for (auto f : all) SafeRelease(*f);
}

static void CreateFormats() {
    ReleaseFormats();
    const wchar_t* ui = UiFont();
    g_fTitle   = Format(DisplayFont(), 28, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fBrand   = Format(DisplayFont(), 18, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fNav     = Format(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fLabel   = Format(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fDesc    = Format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true, true);
    g_fHeader  = Format(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fBtn     = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIcon    = Format(IconFont(), 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fIconBig = Format(IconFont(), 26, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    g_fSmallC  = Format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_fNotes   = Format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
    g_fHero    = Format(DisplayFont(), 34, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_fCard    = Format(ui, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
}

static void Discard() { SafeRelease(g_br); SafeRelease(g_rt); }

static bool EnsureTarget() {
    if (g_rt) return true;
    RECT rc; GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    props.dpiX = props.dpiY = (float)g_dpi;
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom))), &g_rt)))
        return false;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(g_theme.text, &g_br);
    g_cv.rt = g_rt;
    g_cv.br = g_br;
    return g_br != nullptr;
}

static void ApplyTheme() {
    if (!g_hwnd) return;
    g_theme = BuildTheme(g_config);
    Theme win = g_theme;
    win.backdrop = SystemBackdropSupported() ? 2 : 0;   // settings always use Mica when available
    ApplyWindowTheme(g_hwnd, win, false);
    g_theme.backdrop = win.backdrop;
    Discard();
}

// ============================================================ drawing helpers
static void AddZone(int ctl, int part, D2D1_RECT_F rc) { g_zones.push_back({ctl, part, rc}); }
static bool Hot(int ctl, int part) { return g_hotCtl == ctl && g_hotPart == part; }

static float AnimValue(const std::wstring& key, float target) {
    auto it = g_anim.find(key);
    if (it == g_anim.end()) { g_anim[key] = target; return target; }
    return it->second;
}

static void StartAnim() {
    if (!g_animTimer && g_hwnd) { SetTimer(g_hwnd, TIMER_SANIM, 10, nullptr); g_animTimer = true; }
}

static bool StepAnims(std::map<std::wstring, float>& targets) {
    bool moving = false;
    for (auto& kv : targets) {
        float& v = g_anim[kv.first];
        float d = kv.second - v;
        if (std::fabs(d) < 0.01f) v = kv.second;
        else { v += d * 0.35f; moving = true; }
    }
    return moving;
}

static std::map<std::wstring, float> g_animTargets;

static float DrawButton(float right, float cy, const std::wstring& text, bool primary, int ctl, int part, bool enabled = true) {
    float w = MeasureWidth(text, g_fBtn) + 32;
    D2D1_RECT_F r = D2D1::RectF(right - w, cy - 16, right, cy + 16);
    bool hot = Hot(ctl, part) && enabled;
    const Theme& t = g_theme;
    if (primary) {
        g_cv.FillRound(r, 16, Alpha(hot ? Mix(t.accent, D2D1::ColorF(1, 1, 1, 1), 0.12f) : t.accent, enabled ? 1.0f : 0.4f));
        g_cv.Text(text, g_fBtn, r, t.onAccent);
    } else {
        g_cv.FillRound(r, 16, hot ? t.surfaceStrong : Alpha(t.surface, 0.9f));
        g_cv.StrokeRound(r, 16, t.divider);
        g_cv.Text(text, g_fBtn, r, enabled ? t.text : Alpha(t.subtext, 0.6f));
    }
    if (enabled) AddZone(ctl, part, r);
    return w;
}

static float CardHeight(const Ctl& c, float width) {
    switch (c.type) {
        case CType::Header: return 40;
        case CType::ThemeGrid: { int perRow = std::max(1, (int)((width + 12) / 124)); int rows = (kThemePresetCount + perRow - 1) / perRow; return rows * 112.0f; }
        case CType::LayoutGrid: return 150;
        case CType::PinRow: case CType::HiddenRow: case CType::TagRow: return 56;
        case CType::Info: return std::max(52.0f, MeasureHeight(c.desc, g_fDesc, width - 40) + 32);
        case CType::Notes: return MeasureHeight(c.desc, g_fNotes, width - 40) + 32;
        case CType::UpdateCard: return 148;
        case CType::Hero: return 128;
        default: {
            float descH = c.desc.empty() ? 0 : MeasureHeight(c.desc, g_fDesc, width * 0.55f);
            return std::max(56.0f, 30 + descH + 14);
        }
    }
}

static void DrawThemeCard(float x, float y, float w, float h, int idx, int ctl) {
    const ThemePreset& p = kThemePresets[idx];
    Theme t = BuildThemeNamed(g_config, p.id);
    bool sel = g_config.theme == p.id;
    bool hot = Hot(ctl, idx);
    D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h - 26);
    float lift = hot ? -2.0f : 0.0f;
    card.top += lift; card.bottom += lift;
    g_cv.FillRound(card, 10, t.bg);
    if (std::wstring(p.id) == L"system") {
        // Split preview: half light, half dark
        Theme lt = BuildThemeNamed(g_config, L"light");
        g_rt->PushAxisAlignedClip(D2D1::RectF(card.left, card.top, (card.left + card.right) / 2, card.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        g_cv.FillRound(card, 10, lt.bg);
        g_rt->PopAxisAlignedClip();
    }
    if (std::wstring(p.id) == L"glass") g_cv.FillRound(card, 10, Alpha(D2D1::ColorF(0.6f, 0.75f, 1.0f, 1.0f), 0.18f));
    // mini menu: glow, search pill, tiles
    g_cv.Circle(card.left + 18, card.top + 14, 22, Alpha(t.accent, 0.22f));
    g_cv.FillRound(D2D1::RectF(card.left + 10, card.top + 10, card.right - 10, card.top + 20), 5, t.surface);
    for (int i = 0; i < 4; ++i) {
        float tx = card.left + 12 + i * ((w - 24) / 4.0f);
        g_cv.FillRound(D2D1::RectF(tx, card.top + 30, tx + 14, card.top + 44), 4, i == 0 ? t.accent : Alpha(t.subtext, 0.5f));
    }
    g_cv.FillRound(D2D1::RectF(card.left + 12, card.top + 52, card.right - 30, card.top + 56), 2, Alpha(t.subtext, 0.35f));
    g_cv.StrokeRound(card, 10, sel ? g_theme.accent : (hot ? Alpha(g_theme.text, 0.35f) : g_theme.divider), sel ? 2.5f : 1.0f);
    if (sel) {
        g_cv.Circle(card.right - 10, card.top + 10, 8, g_theme.accent);
        g_cv.Glyph(0xE73E, g_fSmallC, D2D1::RectF(card.right - 18, card.top + 2, card.right - 2, card.top + 18), g_theme.onAccent);
    }
    g_cv.Text(p.name, g_fSmallC, D2D1::RectF(x, card.bottom + 4, x + w, y + h), sel ? g_theme.text : g_theme.subtext);
    AddZone(ctl, idx, D2D1::RectF(x, y, x + w, y + h));
}

static D2D1_RECT_F Inset_(const D2D1_RECT_F& r, float d) { return D2D1::RectF(r.left + d, r.top + d, r.right - d, r.bottom - d); }

static void DrawLayoutCard(float x, float y, float w, float h, int idx, int ctl) {
    const wchar_t* ids[] = {L"dual", L"single"};
    const wchar_t* names[] = {L"Dual pane", L"Single pane"};
    bool sel = g_config.layout == ids[idx];
    bool hot = Hot(ctl, idx);
    const Theme& t = g_theme;
    D2D1_RECT_F card = D2D1::RectF(x, y + (hot ? -2.0f : 0.0f), x + w, y + h - 28 + (hot ? -2.0f : 0.0f));
    g_cv.FillRound(card, 10, Alpha(t.surface, 0.9f));
    D2D1_RECT_F s = Inset_(card, 12);
    auto bar = [&](float l, float tp, float r, float b, D2D1_COLOR_F c) { g_cv.FillRound(D2D1::RectF(l, tp, r, b), 3, c); };
    D2D1_COLOR_F dim = Alpha(t.subtext, 0.45f);
    float side = s.left + (s.right - s.left) * 0.2f;
    bar(s.left, s.top, side - 4, s.bottom, Alpha(t.subtext, 0.2f));
    if (idx == 0) {
        float mid = side + (s.right - side) / 2;
        for (int p = 0; p < 2; ++p) {
            float l = p ? mid + 3 : side + 2, r = p ? s.right : mid - 3;
            bar(l, s.top, r, s.top + 6, p == 0 ? Alpha(t.accent, 0.7f) : dim);
            for (int k = 0; k < 6; ++k) bar(l + 2, s.top + 12 + k * 11, r - 6, s.top + 17 + k * 11, (p == 0 && k == 1) ? t.accent : dim);
        }
    } else {
        bar(side + 2, s.top, s.right, s.top + 6, Alpha(t.accent, 0.7f));
        for (int k = 0; k < 6; ++k) bar(side + 4, s.top + 12 + k * 11, s.right - 6, s.top + 17 + k * 11, k == 1 ? t.accent : dim);
    }
    g_cv.StrokeRound(card, 10, sel ? t.accent : (hot ? Alpha(t.text, 0.35f) : t.divider), sel ? 2.5f : 1.0f);
    g_cv.Text(names[idx], g_fSmallC, D2D1::RectF(x, card.bottom + 4, x + w, y + h), sel ? t.text : t.subtext);
    AddZone(ctl, idx, D2D1::RectF(x, y, x + w, y + h));
}

static void DrawUpdateCard(const D2D1_RECT_F& card, int ci) {
    const Theme& t = g_theme;
    updater::Status st = updater::Get();
    using S = updater::State;
    wchar_t glyph = 0xE895;
    D2D1_COLOR_F gc = t.accent;
    std::wstring title = st.message.empty() ? L"Check for updates" : st.message;
    std::wstring sub = L"You're running Strata " STRATA_VERSION;
    if (st.state == S::UpToDate) { glyph = 0xE930; title = L"You're up to date"; }
    if (st.state == S::Error) { glyph = 0xE7BA; gc = D2D1::ColorF(0.95f, 0.55f, 0.35f, 1.0f); }
    if (st.state == S::Available || st.state == S::Ready) { glyph = 0xE896; sub = L"Installed: " STRATA_VERSION L"   \x2192   New: " + st.release.version + (st.release.prerelease ? L" (beta)" : L""); }
    if (g_config.lastCheck > 0 && (st.state == S::UpToDate || st.state == S::Idle || st.state == S::Error)) {
        time_t tt = (time_t)g_config.lastCheck;
        tm lt{}; localtime_s(&lt, &tt);
        wchar_t b[64]; wcsftime(b, 64, L"%b %d, %I:%M %p", &lt);
        sub += L"   \x00B7   Last checked " + std::wstring(b);
    }
    D2D1_RECT_F badge = D2D1::RectF(card.left + 20, card.top + 22, card.left + 72, card.top + 74);
    g_cv.FillRound(badge, 14, Alpha(gc, 0.16f));
    bool spinning = st.state == S::Checking || st.state == S::Downloading || st.state == S::Verifying || st.state == S::Installing;
    if (spinning) {
        // simple animated arc: rotate the sync glyph
        float angle = (float)fmod(GetTickCount64() / 4.0, 360.0);
        D2D1_MATRIX_3X2_F saved; g_rt->GetTransform(&saved);
        g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F((badge.left + badge.right) / 2, (badge.top + badge.bottom) / 2)) * saved);
        g_cv.Glyph(0xE895, g_fIconBig, badge, gc);
        g_rt->SetTransform(saved);
        g_animTargets[L"__spin"] = (float)GetTickCount64();   // keeps the timer alive
    } else {
        g_cv.Glyph(glyph, g_fIconBig, badge, gc);
    }
    g_cv.Text(title, g_fCard, D2D1::RectF(card.left + 88, card.top + 22, card.right - 20, card.top + 48), t.text);
    g_cv.Text(sub, g_fDesc, D2D1::RectF(card.left + 88, card.top + 50, card.right - 20, card.top + 72), t.subtext);
    if (st.state == S::Downloading) {
        D2D1_RECT_F track = D2D1::RectF(card.left + 88, card.top + 80, card.right - 24, card.top + 84);
        g_cv.FillRound(track, 2, Alpha(t.subtext, 0.25f));
        g_cv.FillRound(D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * st.percent / 100.0f, track.bottom), 2, t.accent);
    }
    float bx = card.right - 20, by = card.bottom - 32;
    if (st.state == S::Ready) {
        bx -= DrawButton(bx, by, L"Restart and update", true, ci, 2) + 8;
    } else if (st.state == S::Available || (st.state == S::Error && !st.release.exeUrl.empty())) {
        bx -= DrawButton(bx, by, L"Download and install", true, ci, 1) + 8;
    }
    if (!st.release.page.empty()) bx -= DrawButton(bx, by, L"View on GitHub", false, ci, 3) + 8;
    DrawButton(bx, by, L"Check now", st.state != S::Available && st.state != S::Ready, ci, 0, !spinning);
}

static void Paint() {
    if (!EnsureTarget()) return;
    const Theme& t = g_theme;
    RECT rc; GetClientRect(g_hwnd, &rc);
    float W = Dip(rc.right), H = Dip(rc.bottom);
    g_zones.clear();
    g_animTargets.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    bool glass = t.backdrop != 0;

    // Sidebar
    g_cv.Fill(D2D1::RectF(0, 0, kSidebarW, H), glass ? Alpha(t.bg, 0.25f) : t.bottomBg);
    g_cv.Fill(D2D1::RectF(kSidebarW, 0, W, H), glass ? Alpha(t.bg, 0.45f) : t.bg);
    D2D1_RECT_F mark = D2D1::RectF(22, 22, 54, 54);
    g_cv.FillRound(mark, 9, t.accent);
    // Strata's mark: three layers
    for (int k = 0; k < 3; ++k) {
        float yy = mark.top + 8 + k * 6.5f;
        g_cv.FillRound(D2D1::RectF(mark.left + 7 + (2 - k) * 1.5f, yy, mark.right - 7 - k * 2.0f, yy + 4), 2, Alpha(t.onAccent, 0.55f + k * 0.2f));
    }
    g_cv.Text(L"Strata", g_fBrand, D2D1::RectF(64, 18, kSidebarW - 10, 40), t.text);
    g_cv.Text(L"Version " STRATA_VERSION, g_fDesc, D2D1::RectF(64, 40, kSidebarW - 10, 58), t.subtext);
    float ny = 84;
    for (int p = 0; p < PageCount; ++p) {
        D2D1_RECT_F r = D2D1::RectF(12, ny, kSidebarW - 12, ny + 40);
        bool sel = p == g_page;
        if (sel) g_cv.FillRound(r, 6, Alpha(t.surface, glass ? 0.8f : 1.0f));
        else if (Hot(-2, p)) g_cv.FillRound(r, 6, t.hover);
        if (sel) g_cv.FillRound(D2D1::RectF(r.left + 2, r.top + 11, r.left + 5, r.bottom - 11), 1.5f, t.accent);
        g_cv.Glyph(kPageGlyphs[p], g_fIcon, D2D1::RectF(r.left + 12, r.top, r.left + 40, r.bottom), sel ? t.accent : t.text);
        g_cv.Text(kPageNames[p], g_fNav, D2D1::RectF(r.left + 48, r.top, r.right - 8, r.bottom), t.text);
        if (p == PageUpdates && AutoUpdateAvailable()) g_cv.Circle(r.right - 16, (r.top + r.bottom) / 2, 4, t.accent);
        AddZone(-2, p, r);
        ny += 44;
    }

    // Content
    float cx0 = kSidebarW + 36, cx1 = W - 36;
    float width = cx1 - cx0;
    g_cv.Text(kPageNames[g_page], g_fTitle, D2D1::RectF(cx0, 22, cx1, 64), t.text);
    float top = 80;
    g_viewH = H - top;
    D2D1_RECT_F clip = D2D1::RectF(kSidebarW, top, W, H);
    g_rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = top - g_scroll;
    for (int i = 0; i < (int)g_ctls.size(); ++i) {
        Ctl& c = g_ctls[i];
        float h = CardHeight(c, width);
        D2D1_RECT_F card = D2D1::RectF(cx0, y, cx1, y + h);
        if (y + h < top - 4 || y > H + 4) { y += h + (c.type == CType::Header ? 0 : 4); continue; }
        float alpha = c.enabled ? 1.0f : 0.5f;
        bool isCard = c.type != CType::Header && c.type != CType::ThemeGrid && c.type != CType::LayoutGrid && c.type != CType::Hero;
        if (isCard) {
            g_cv.FillRound(card, std::min(8.0f, std::max(4.0f, t.radius)), Alpha(t.surface, glass ? 0.72f : 1.0f));
            g_cv.StrokeRound(card, std::min(8.0f, std::max(4.0f, t.radius)), Alpha(t.divider, 0.8f));
        }
        float midY = (card.top + card.bottom) / 2;
        D2D1_RECT_F labelR = D2D1::RectF(card.left + 20, card.top + 12, card.left + width * 0.58f, card.top + 32);
        D2D1_RECT_F descR = D2D1::RectF(card.left + 20, card.top + 33, card.left + width * 0.58f, card.bottom - 8);
        if (c.desc.empty() && (c.type == CType::Toggle || c.type == CType::Choice || c.type == CType::Slider || c.type == CType::Buttons || c.type == CType::Swatches))
            labelR = D2D1::RectF(card.left + 20, card.top, card.left + width * 0.58f, card.bottom);

        switch (c.type) {
            case CType::Header:
                g_cv.Text(c.label, g_fHeader, D2D1::RectF(cx0 + 2, card.top + 14, cx1, card.bottom - 2), t.text);
                break;
            case CType::Toggle: {
                g_cv.Text(c.label, g_fLabel, labelR, Alpha(t.text, alpha));
                g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                bool on = c.getb ? c.getb() : false;
                float k = AnimValue(c.label, on ? 1.0f : 0.0f);
                g_animTargets[c.label] = on ? 1.0f : 0.0f;
                D2D1_RECT_F tr = D2D1::RectF(cx1 - 64, midY - 11, cx1 - 20, midY + 11);
                bool hot = Hot(i, 0);
                g_cv.FillRound(tr, 11, Mix(Alpha(t.surfaceStrong, 1.0f), t.accent, k));
                if (k < 0.99f) g_cv.StrokeRound(tr, 11, Alpha(t.subtext, (1 - k) * 0.8f));
                float knob = tr.left + 11 + k * (tr.right - tr.left - 22);
                g_cv.Circle(knob, midY, hot ? 7.5f : 6.5f, Mix(t.subtext, t.onAccent, k));
                g_cv.Text(on ? L"On" : L"Off", g_fDesc, D2D1::RectF(tr.left - 44, midY - 10, tr.left - 10, midY + 10), t.subtext);
                AddZone(i, 0, D2D1::RectF(tr.left - 50, card.top, cx1, card.bottom));
                break;
            }
            case CType::Choice: {
                g_cv.Text(c.label, g_fLabel, labelR, Alpha(t.text, alpha));
                g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                int sel = c.geti ? c.geti() : 0;
                float total = 6;
                std::vector<float> ws;
                for (auto& o : c.options) { float w = MeasureWidth(o, g_fBtn) + 26; ws.push_back(w); total += w; }
                float x = cx1 - 20 - total;
                D2D1_RECT_F pill = D2D1::RectF(x, midY - 17, cx1 - 20, midY + 17);
                g_cv.FillRound(pill, 17, Alpha(t.surfaceStrong, alpha));
                x += 3;
                for (int o = 0; o < (int)c.options.size(); ++o) {
                    D2D1_RECT_F r = D2D1::RectF(x, midY - 14, x + ws[o], midY + 14);
                    if (o == sel) g_cv.FillRound(r, 14, Alpha(t.accent, alpha));
                    else if (Hot(i, o)) g_cv.FillRound(r, 14, t.hover);
                    g_cv.Text(c.options[o], g_fBtn, r, o == sel ? t.onAccent : Alpha(t.text, alpha));
                    if (c.enabled) AddZone(i, o, r);
                    x += ws[o];
                }
                break;
            }
            case CType::Slider: {
                g_cv.Text(c.label, g_fLabel, labelR, Alpha(t.text, alpha));
                g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                float v = c.getf ? c.getf() : 0;
                float f = std::clamp((v - c.fmin) / (c.fmax - c.fmin), 0.0f, 1.0f);
                D2D1_RECT_F tr = D2D1::RectF(cx1 - 220, midY - 2, cx1 - 24, midY + 2);
                g_cv.FillRound(tr, 2, Alpha(t.subtext, 0.35f * alpha));
                float kx = tr.left + f * (tr.right - tr.left);
                g_cv.FillRound(D2D1::RectF(tr.left, tr.top, kx, tr.bottom), 2, Alpha(t.accent, alpha));
                bool hot = Hot(i, 0) || g_dragCtl == i;
                g_cv.Circle(kx, midY, 10, Alpha(t.surface, 1.0f));
                g_cv.Ring(kx, midY, 10, Alpha(t.divider, 1.0f), 1.0f);
                g_cv.Circle(kx, midY, hot ? 6.5f : 5.0f, Alpha(t.accent, alpha));
                if (c.fmt) g_cv.Text(c.fmt(v), g_fDesc, D2D1::RectF(tr.left - 70, midY - 10, tr.left - 14, midY + 10), t.subtext);
                if (c.enabled) AddZone(i, 0, D2D1::RectF(tr.left - 10, card.top, tr.right + 10, card.bottom));
                break;
            }
            case CType::Swatches: {
                g_cv.Text(c.label, g_fLabel, labelR, t.text);
                g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                static const uint32_t colors[] = {0, 0x4CC2FF, 0x7C83FD, 0xC86DD7, 0xFF6B9A, 0xFF7A45, 0xF5B83D, 0x3DD68C, 0x2EC4B6};
                const int n = (int)(sizeof(colors) / sizeof(colors[0]));
                float x = cx1 - 24 - n * 30;
                for (int s = 0; s < n; ++s) {
                    uint32_t col = s == 0 ? WindowsAccentRgb() : colors[s];
                    bool sel = s == 0 ? g_config.useWindowsAccent : (!g_config.useWindowsAccent && g_config.accentColor == colors[s]);
                    float scx = x + s * 30 + 15;
                    if (sel) g_cv.Ring(scx, midY, 14, t.text, 2.0f);
                    else if (Hot(i, s)) g_cv.Ring(scx, midY, 14, Alpha(t.text, 0.4f), 1.5f);
                    g_cv.Circle(scx, midY, 10.5f, Rgb(col));
                    if (s == 0) g_cv.Glyph(0xE770, g_fSmallC, D2D1::RectF(scx - 8, midY - 8, scx + 8, midY + 8), D2D1::ColorF(1, 1, 1, 0.9f));
                    AddZone(i, s, D2D1::RectF(scx - 15, midY - 15, scx + 15, midY + 15));
                }
                break;
            }
            case CType::Buttons: {
                if (!c.label.empty()) {
                    g_cv.Text(c.label, g_fLabel, labelR, t.text);
                    g_cv.Text(c.desc, g_fDesc, descR, t.subtext);
                }
                float bx = cx1 - 20;
                for (int b = (int)c.buttons.size() - 1; b >= 0; --b)
                    bx -= DrawButton(bx, midY, c.buttons[b].first, b == c.primary, i, b) + 8;
                break;
            }
            case CType::Info:
                g_cv.Text(c.desc, g_fDesc, D2D1::RectF(card.left + 20, card.top + 16, card.right - 20, card.bottom - 8), t.subtext);
                break;
            case CType::Notes:
                g_cv.Text(c.desc, g_fNotes, D2D1::RectF(card.left + 20, card.top + 16, card.right - 20, card.bottom - 8), t.text);
                break;
            case CType::ThemeGrid: {
                int perRow = std::max(1, (int)((width + 12) / 124));
                for (int p = 0; p < kThemePresetCount; ++p)
                    DrawThemeCard(cx0 + (p % perRow) * 124, card.top + (p / perRow) * 112, 112, 104, p, i);
                break;
            }
            case CType::LayoutGrid: {
                float w = std::min(220.0f, (width - 12) / 2);
                for (int p = 0; p < 2; ++p) DrawLayoutCard(cx0 + p * (w + 12), card.top + 4, w, 144, p, i);
                break;
            }
            case CType::TagRow: {
                const TagDef& tg = g_config.tags[c.index];
                g_cv.Circle(card.left + 28, midY, 8, Rgb(tg.color));
                g_cv.Text(c.label, g_fLabel, D2D1::RectF(card.left + 50, card.top + 8, cx1 - 300, midY + 2), t.text);
                g_cv.Text(c.desc, g_fDesc, D2D1::RectF(card.left + 50, midY + 2, cx1 - 300, card.bottom - 6), t.subtext);
                float bx = cx1 - 20;
                bx -= DrawButton(bx, midY, L"Delete", false, i, 2) + 8;
                bx -= DrawButton(bx, midY, L"Rename", false, i, 1) + 8;
                DrawButton(bx, midY, L"Color", false, i, 0);
                break;
            }
            case CType::PinRow:
            case CType::HiddenRow: {
                bool pin = c.type == CType::PinRow;
                g_cv.Glyph(pin ? 0xE718 : 0xE721, g_fIcon, D2D1::RectF(card.left + 12, card.top, card.left + 44, card.bottom), t.subtext);
                g_cv.Text(c.label, g_fLabel, D2D1::RectF(card.left + 50, card.top + 8, cx1 - 200, midY + 2), t.text);
                g_cv.Text(c.desc, g_fDesc, D2D1::RectF(card.left + 50, midY + 2, cx1 - 200, card.bottom - 6), t.subtext);
                if (pin) {
                    const wchar_t glyphs[3] = {0xE70E, 0xE70D, 0xE711};
                    for (int b = 0; b < 3; ++b) {
                        D2D1_RECT_F r = D2D1::RectF(cx1 - 20 - (3 - b) * 40, midY - 16, cx1 - 20 - (3 - b) * 40 + 36, midY + 16);
                        bool enabled = !(b == 0 && c.index == 0) && !(b == 1 && c.index + 1 >= (int)g_config.pins.size());
                        if (Hot(i, b) && enabled) g_cv.FillRound(r, 6, t.hover);
                        g_cv.Glyph(glyphs[b], g_fIcon, r, enabled ? (b == 2 ? D2D1::ColorF(0.95f, 0.45f, 0.4f, 1) : t.text) : Alpha(t.subtext, 0.4f));
                        if (enabled) AddZone(i, b, r);
                    }
                } else {
                    DrawButton(cx1 - 20, midY, L"Remove", false, i, 0);
                }
                break;
            }
            case CType::UpdateCard:
                DrawUpdateCard(card, i);
                break;
            case CType::Hero: {
                D2D1_RECT_F big = D2D1::RectF(cx0, card.top + 10, cx0 + 92, card.top + 102);
                g_cv.Circle(cx0 + 46, card.top + 56, 70, Alpha(t.accent, 0.12f));
                g_cv.FillRound(big, 24, t.accent);
                for (int k = 0; k < 3; ++k) {
                    float yy = big.top + 24 + k * 16;
                    g_cv.FillRound(D2D1::RectF(big.left + 18 + (2 - k) * 4, yy, big.right - 18 - k * 5, yy + 10), 5, Alpha(t.onAccent, 0.55f + k * 0.2f));
                }
                g_cv.Text(L"Strata", g_fHero, D2D1::RectF(cx0 + 116, card.top + 12, cx1, card.top + 56), t.text);
                g_cv.Text(L"Version " STRATA_VERSION L"  \x00B7  A fast, modern file manager. Part of the larosa023 suite.", g_fDesc,
                          D2D1::RectF(cx0 + 118, card.top + 60, cx1, card.top + 96), t.subtext);
                break;
            }
        }
        y += h + (c.type == CType::Header ? 0 : 4);
    }
    g_contentH = y + g_scroll - top + 24;
    g_rt->PopAxisAlignedClip();

    // scrollbar
    if (g_contentH > g_viewH) {
        float thumbH = std::max(30.0f, g_viewH * g_viewH / g_contentH);
        float ty = top + (g_scroll / (g_contentH - g_viewH)) * (g_viewH - thumbH);
        g_cv.FillRound(D2D1::RectF(W - 8, ty, W - 4, ty + thumbH), 2, Alpha(t.subtext, 0.4f));
    }

    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { Discard(); InvalidateRect(g_hwnd, nullptr, FALSE); }

    // Keep animating while toggles slide or something spins.
    bool moving = false;
    for (auto& kv : g_animTargets) {
        if (kv.first == L"__spin") { moving = true; continue; }
        auto it = g_anim.find(kv.first);
        if (it != g_anim.end() && std::fabs(it->second - kv.second) > 0.01f) moving = true;
    }
    if (moving) StartAnim();
}

// ============================================================ interaction
static void SetScroll(float s) {
    g_scroll = std::clamp(s, 0.0f, std::max(0.0f, g_contentH - g_viewH));
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void GoToPage(int p) {
    if (p < 0 || p >= PageCount) return;
    g_page = p;
    g_scroll = 0;
    BuildPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void SliderSet(int ci, float x) {
    Ctl& c = g_ctls[ci];
    for (const Zone& z : g_zones) {
        if (z.ctl != ci) continue;
        float l = z.rc.left + 10, r = z.rc.right - 10;
        float f = std::clamp((x - l) / (r - l), 0.0f, 1.0f);
        if (c.setf) c.setf(c.fmin + f * (c.fmax - c.fmin));
        break;
    }
}

static void Click(int ci, int part, float x) {
    if (ci == -2) { GoToPage(part); return; }
    if (ci < 0 || ci >= (int)g_ctls.size()) return;
    Ctl& c = g_ctls[ci];
    switch (c.type) {
        case CType::Toggle: if (c.setb) c.setb(!c.getb()); StartAnim(); break;
        case CType::Choice: if (c.seti) c.seti(part); break;
        case CType::Slider: SliderSet(ci, x); break;
        case CType::Swatches: {
            static const uint32_t colors[] = {0, 0x4CC2FF, 0x7C83FD, 0xC86DD7, 0xFF6B9A, 0xFF7A45, 0xF5B83D, 0x3DD68C, 0x2EC4B6};
            if (part == 0) g_config.useWindowsAccent = true;
            else { g_config.useWindowsAccent = false; g_config.accentColor = colors[part]; }
            Changed();
            break;
        }
        case CType::Buttons: if (part < (int)c.buttons.size() && c.buttons[part].second) c.buttons[part].second(); break;
        case CType::ThemeGrid: g_config.theme = kThemePresets[part].id; g_config.customColors.clear(); Changed(); break;
        case CType::LayoutGrid: g_config.layout = part ? L"single" : L"dual"; Changed(); break;
        case CType::PinRow: {
            int i = c.index;
            if (part == 0 && i > 0) std::swap(g_config.pins[i], g_config.pins[i - 1]);
            else if (part == 1 && i + 1 < (int)g_config.pins.size()) std::swap(g_config.pins[i], g_config.pins[i + 1]);
            else if (part == 2) g_config.pins.erase(g_config.pins.begin() + i);
            Changed();
            break;
        }
        case CType::HiddenRow:
            if (c.index < (int)g_config.savedSearches.size()) g_config.savedSearches.erase(g_config.savedSearches.begin() + c.index);
            Changed();
            break;
        case CType::TagRow: {
            TagDef& tg = g_config.tags[c.index];
            if (part == 0) {
                static COLORREF custom[16] = {};
                CHOOSECOLORW cc{sizeof(cc)};
                cc.hwndOwner = g_hwnd;
                cc.rgbResult = RGB((tg.color >> 16) & 0xFF, (tg.color >> 8) & 0xFF, tg.color & 0xFF);
                cc.lpCustColors = custom;
                cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (ChooseColorW(&cc)) tg.color = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
            } else if (part == 1) {
                std::wstring name = tg.name;
                if (TextDialog(g_hwnd, L"Rename tag", L"", name, L"Rename") && !name.empty()) tg.name = name;
            } else if (part == 2) {
                size_t n = TagsFind(tg.id).size();
                std::wstring q = L"Delete the tag \"" + tg.name + L"\"?" + (n ? L"\n\nIt will be removed from " + std::to_wstring(n) + L" item(s)." : L"");
                if (ConfirmDialog(g_hwnd, L"Delete the tag \"" + tg.name + L"\"?", n ? L"It will be removed from " + std::to_wstring(n) + L" item(s)." : L"",
                                  L"Delete tag", true)) {
                    TagsRemoveTag(tg.id);
                    g_config.tags.erase(g_config.tags.begin() + c.index);
                }
            }
            Changed();
            break;
        }
        case CType::UpdateCard:
            if (part == 0) AutoUpdateCheckNow();
            else if (part == 1) AutoUpdateDownloadNow();
            else if (part == 2) AutoUpdateInstallNow();
            else if (part == 3) ShellOpen(updater::Get().release.page.c_str());
            break;
        default: break;
    }
    BuildPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static bool HitZone(float x, float y, int* ci, int* part) {
    for (auto it = g_zones.rbegin(); it != g_zones.rend(); ++it) {
        if (InR(it->rc, x, y)) {
            // zones in the content area are clipped to the visible region
            if (it->ctl != -2 && y < 80) continue;
            *ci = it->ctl; *part = it->part;
            return true;
        }
    }
    return false;
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); Paint(); EndPaint(hwnd, &ps); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            SetScroll(g_scroll);
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wParam);
            RECT* r = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Discard();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lParam;
            mm->ptMinTrackSize = {Px(760), Px(540)};
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_SANIM) {
                bool spin = g_animTargets.count(L"__spin") > 0;
                std::map<std::wstring, float> targets = g_animTargets;
                targets.erase(L"__spin");
                if (!StepAnims(targets) && !spin) { KillTimer(hwnd, TIMER_SANIM); g_animTimer = false; }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            if (!g_tracking) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t); g_tracking = true; }
            if (g_dragCtl >= 0) { SliderSet(g_dragCtl, x); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            int ci = -1, part = -1;
            HitZone(x, y, &ci, &part);
            if (ci != g_hotCtl || part != g_hotPart) { g_hotCtl = ci; g_hotPart = part; InvalidateRect(hwnd, nullptr, FALSE); }
            SetCursor(LoadCursorW(nullptr, ci == -1 && part == -1 ? IDC_ARROW : IDC_HAND));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, (g_hotCtl != -1 || g_hotPart != -1) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE: g_tracking = false; g_hotCtl = g_hotPart = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            int ci = -1, part = -1;
            if (HitZone(x, y, &ci, &part) && ci >= 0 && g_ctls[ci].type == CType::Slider) {
                g_dragCtl = ci;
                SetCapture(hwnd);
                SliderSet(ci, x);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            float x = Dip((short)LOWORD(lParam)), y = Dip((short)HIWORD(lParam));
            if (g_dragCtl >= 0) {
                g_dragCtl = -1;
                ReleaseCapture();
                ConfigSave();   // sliders save once, when released
                Changed(false);
                return 0;
            }
            int ci = -1, part = -1;
            if (HitZone(x, y, &ci, &part)) Click(ci, part, x);
            return 0;
        }
        case WM_MOUSEWHEEL:
            SetScroll(g_scroll - GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * 60);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
            if (wParam == VK_OEM_COMMA && GetKeyState(VK_CONTROL) < 0) { DestroyWindow(hwnd); return 0; }
            if (wParam == VK_PRIOR) { SetScroll(g_scroll - g_viewH * 0.9f); return 0; }
            if (wParam == VK_NEXT) { SetScroll(g_scroll + g_viewH * 0.9f); return 0; }
            if (wParam == VK_TAB && GetKeyState(VK_CONTROL) < 0) { GoToPage((g_page + (GetKeyState(VK_SHIFT) < 0 ? PageCount - 1 : 1)) % PageCount); return 0; }
            break;
        case WM_SETTINGCHANGE:
            ApplyTheme();
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_DESTROY:
            Discard();
            g_hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void SettingsOnUpdateStatus() {
    if (!SettingsIsOpen()) return;
    BuildPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SettingsOpen(int page) {
    if (SettingsIsOpen()) {
        if (page >= 0) GoToPage(page);
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        return;
    }
    gfx::Init();
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        wc.lpszClassName = L"StrataSettings";
        RegisterClassExW(&wc);
        registered = true;
    }
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    g_dpi = dx;
    int w = std::min<int>(Px(980), mi.rcWork.right - mi.rcWork.left - 40);
    int h = std::min<int>(Px(700), mi.rcWork.bottom - mi.rcWork.top - 40);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    if (page >= 0) g_page = page;
    g_scroll = 0;
    CreateFormats();
    g_hwnd = CreateWindowExW(0, L"StrataSettings", L"Strata Settings", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return;
    g_dpi = GetDpiForWindow(g_hwnd);
    ApplyTheme();
    BuildPage();
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    if (g_page == PageUpdates && updater::Get().state == updater::State::Idle) AutoUpdateCheckNow();
}
