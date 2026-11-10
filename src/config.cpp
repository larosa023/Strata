// Strata - settings persisted as config.json (+ views.json for per-folder views)
#include "config.h"
#include "json.h"
#include "tags.h"
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>

Config g_config;

std::wstring ConfigPath() { return DataDir() + L"\\config.json"; }
static std::wstring ViewsPath() { return DataDir() + L"\\views.json"; }

bool ReadUtf8File(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size > 32 * 1024 * 1024) { CloseHandle(h); return false; }
    std::string buf(size, '\0');
    DWORD read = 0;
    bool ok = size == 0 || ReadFile(h, &buf[0], size, &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    size_t skip = (read >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, buf.data() + skip, (int)(read - skip), nullptr, 0);
    out.assign(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, buf.data() + skip, (int)(read - skip), &out[0], n);
    return true;
}

bool WriteUtf8FileAtomic(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string buf(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &buf[0], n, nullptr, nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &written, nullptr) && written == buf.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

uint32_t ParseHexColor(const std::wstring& s, uint32_t def) {
    std::wstring t = s;
    if (!t.empty() && t[0] == L'#') t = t.substr(1);
    if (t.size() != 6) return def;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(t.c_str(), &end, 16);
    return (end && *end == 0) ? (uint32_t)v : def;
}

std::wstring HexColor(uint32_t c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%06X", c & 0xFFFFFF);
    return b;
}

static std::wstring OneOf(const std::wstring& v, std::initializer_list<const wchar_t*> allowed, const wchar_t* def) {
    for (auto a : allowed) if (v == a) return v;
    return def;
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static void DefaultPins() {
    g_config.pins.clear();
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Pictures};
    for (auto id : ids) {
        std::wstring p = KnownFolder(*id);
        if (!p.empty()) g_config.pins.push_back({p, L"", 0});
    }
}

void ConfigDefaultTags() {
    g_config.tags = {
        {L"urgent", L"Urgent", 0xE5484D}, {L"active", L"Active", 0x3E8EF7}, {L"done", L"Done", 0x30A46C}, {L"repo", L"Repo", 0x8E4EC6},
    };
}

// ---------------------------------------------------------------- simple fields (table driven)
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct BoolField { const wchar_t* key; bool Config::*m; };
}  // namespace
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct StrField { const wchar_t* key; std::wstring Config::*m; std::initializer_list<const wchar_t*> allowed; };
}  // namespace
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct FloatField { const wchar_t* key; float Config::*m; float lo, hi; };
}  // namespace

static const BoolField kBools[] = {
    {L"useWindowsAccent", &Config::useWindowsAccent}, {L"accentGlow", &Config::accentGlow},
    {L"respectHighContrast", &Config::respectHighContrast}, {L"showSidebar", &Config::showSidebar},
    {L"showPreview", &Config::showPreview}, {L"showStatusBar", &Config::showStatusBar}, {L"showNavButtons", &Config::showNavButtons},
    {L"showViewSwitch", &Config::showViewSwitch}, {L"sidebarRecent", &Config::sidebarRecent}, {L"sidebarTags", &Config::sidebarTags},
    {L"sidebarSaved", &Config::sidebarSaved}, {L"showHidden", &Config::showHidden}, {L"showExtensions", &Config::showExtensions},
    {L"foldersFirst", &Config::foldersFirst}, {L"rememberFolderViews", &Config::rememberFolderViews}, {L"folderSizes", &Config::folderSizes},
    {L"folderSizesNetwork", &Config::folderSizesNetwork}, {L"confirmDelete", &Config::confirmDelete}, {L"verifyCopies", &Config::verifyCopies},
    {L"skipErrors", &Config::skipErrors}, {L"useEverything", &Config::useEverything}, {L"useIndex", &Config::useIndex},
    {L"searchNetwork", &Config::searchNetwork}, {L"keepInTray", &Config::keepInTray}, {L"defaultFileManager", &Config::defaultFileManager},
    {L"autoCheck", &Config::autoCheck}, {L"autoInstall", &Config::autoInstall}, {L"restoreSession", &Config::restoreSession},
};

static const StrField kStrs[] = {
    {L"theme", &Config::theme, {L"dark", L"light", L"midnight", L"glass", L"ember", L"ocean", L"system", L"custom"}},
    {L"backdrop", &Config::backdrop, {L"solid", L"mica", L"acrylic"}},
    {L"density", &Config::density, {L"compact", L"comfortable"}},
    {L"iconSize", &Config::iconSize, {L"small", L"medium", L"large"}},
    {L"animation", &Config::animation, {L"none", L"fast", L"smooth"}},
    {L"layout", &Config::layout, {L"dual", L"single"}},
    {L"conflictDefault", &Config::conflictDefault, {L"ask", L"skip", L"replace", L"keepboth"}},
    {L"channel", &Config::channel, {L"stable", L"beta"}},
    {L"customTheme", &Config::customTheme, {}},
    {L"fontFamily", &Config::fontFamily, {}},
    {L"notifiedVersion", &Config::notifiedVersion, {}},
};

static const FloatField kFloats[] = {
    {L"opacity", &Config::opacity, 0.4f, 1.0f}, {L"cornerRadius", &Config::cornerRadius, 0, 16}, {L"fontSize", &Config::fontSize, 11, 16},
    {L"paneRatio", &Config::paneRatio, 0.2f, 0.8f}, {L"sidebarWidth", &Config::sidebarWidth, 150, 480},
    {L"previewWidth", &Config::previewWidth, 220, 700},
};

static const wchar_t* kColNames[ColCount] = {L"name", L"size", L"modified", L"type"};

static std::vector<std::wstring> StrList(const json::Value* v) {
    std::vector<std::wstring> out;
    if (v && v->type == json::Value::Array)
        for (auto& x : v->arr) if (x.type == json::Value::String) out.push_back(x.s);
    return out;
}

static json::Value StrArray(const std::vector<std::wstring>& v) {
    json::Value a = json::Value::MakeArray();
    for (auto& s : v) a.arr.push_back(json::Value::Str(s));
    return a;
}

static void FromJson(const json::Value& root) {
    Config d;
    Config& c = g_config;
    for (auto& f : kBools) c.*f.m = root.GetBool(f.key, d.*f.m);
    for (auto& f : kStrs) {
        std::wstring v = root.GetStr(f.key, d.*f.m);
        if (f.allowed.size()) {
            bool ok = false;
            for (auto a : f.allowed) if (v == a) ok = true;
            if (!ok) v = d.*f.m;
        }
        c.*f.m = v;
    }
    for (auto& f : kFloats) c.*f.m = std::clamp((float)root.GetNum(f.key, d.*f.m), f.lo, f.hi);
    c.accentColor = ParseHexColor(root.GetStr(L"accentColor"), d.accentColor);
    c.checkHours = std::clamp((int)root.GetNum(L"checkHours", d.checkHours), 1, 168);
    c.lastCheck = root.GetNum(L"lastCheck", 0);

    c.customColors.clear();
    if (const json::Value* cc = root.Get(L"customColors"); cc && cc->type == json::Value::Object)
        for (auto& kv : cc->obj) if (kv.second.type == json::Value::String) c.customColors.push_back({kv.first, ParseHexColor(kv.second.s, 0)});

    if (const json::Value* cols = root.Get(L"columns"); cols && cols->type == json::Value::Object) {
        for (int i = 0; i < ColCount; ++i) {
            const json::Value* col = cols->Get(kColNames[i]);
            if (!col) continue;
            c.columnWidth[i] = std::clamp((float)col->GetNum(L"width", d.columnWidth[i]), 48.0f, 2000.0f);
            c.columnVisible[i] = i == ColName ? true : col->GetBool(L"visible", true);
        }
    }
    std::wstring sort = root.GetStr(L"sortBy", L"name");
    c.sortColumn = ColName;
    for (int i = 0; i < ColCount; ++i) if (sort == kColNames[i]) c.sortColumn = i;
    c.sortDescending = root.GetBool(L"sortDescending", false);
    c.recentFolders = StrList(root.Get(L"recentFolders"));
    c.hiddenMenuItems = StrList(root.Get(L"hiddenMenuItems"));
    c.seenMenuItems = StrList(root.Get(L"seenMenuItems"));

    if (const json::Value* w = root.Get(L"window"); w && w->type == json::Value::Object) {
        c.windowRect = {(LONG)w->GetNum(L"left", 0), (LONG)w->GetNum(L"top", 0), (LONG)w->GetNum(L"right", 0),
                        (LONG)w->GetNum(L"bottom", 0)};
        c.maximized = w->GetBool(L"maximized", false);
    }
    c.session.clear();
    if (const json::Value* s = root.Get(L"session"); s && s->type == json::Value::Object) {
        if (const json::Value* tabs = s->Get(L"tabs"); tabs && tabs->type == json::Value::Array) {
            for (auto& v : tabs->arr) {
                if (v.type == json::Value::String) c.session.push_back({v.s, v.s, 0});   // 0.1.x format
                else if (v.type == json::Value::Object)
                    c.session.push_back({v.GetStr(L"left"), v.GetStr(L"right"), std::clamp((int)v.GetNum(L"active", 0), 0, 1)});
            }
        }
        c.sessionActive = (int)s->GetNum(L"active", 0);
    }

    c.pins.clear();
    if (const json::Value* pins = root.Get(L"pins"); pins && pins->type == json::Value::Array) {
        for (auto& v : pins->arr) {
            Pin pin{v.GetStr(L"path"), v.GetStr(L"name"), ParseHexColor(v.GetStr(L"color"), 0)};
            if (!pin.path.empty()) c.pins.push_back(pin);
        }
    }
    if (const json::Value* tags = root.Get(L"tags"); tags && tags->type == json::Value::Array) {
        c.tags.clear();
        for (auto& v : tags->arr) {
            TagDef t{v.GetStr(L"id"), v.GetStr(L"name"), ParseHexColor(v.GetStr(L"color"), 0x888888)};
            if (!t.id.empty()) c.tags.push_back(t);
        }
    } else {
        ConfigDefaultTags();
    }
    c.savedSearches.clear();
    if (const json::Value* ss = root.Get(L"savedSearches"); ss && ss->type == json::Value::Array)
        for (auto& v : ss->arr) {
            SavedSearch s{v.GetStr(L"name"), v.GetStr(L"root"), v.GetStr(L"query")};
            if (!s.query.empty()) c.savedSearches.push_back(s);
        }
}

static json::Value ToJson() {
    const Config& c = g_config;
    using json::Value;
    Value root = Value::MakeObject();
    root.Set(L"version", Value::Num(2));
    for (auto& f : kStrs) root.Set(f.key, Value::Str(c.*f.m));
    for (auto& f : kBools) root.Set(f.key, Value::Boolean(c.*f.m));
    for (auto& f : kFloats) root.Set(f.key, Value::Num(std::round(c.*f.m * 100) / 100));
    root.Set(L"accentColor", Value::Str(HexColor(c.accentColor)));
    root.Set(L"checkHours", Value::Num(c.checkHours));
    root.Set(L"lastCheck", Value::Num(c.lastCheck));
    Value cc = Value::MakeObject();
    for (auto& kv : c.customColors) cc.Set(kv.first, Value::Str(HexColor(kv.second)));
    root.Set(L"customColors", cc);

    Value cols = Value::MakeObject();
    for (int i = 0; i < ColCount; ++i) {
        Value col = Value::MakeObject();
        col.Set(L"width", Value::Num(std::round(c.columnWidth[i])));
        col.Set(L"visible", Value::Boolean(c.columnVisible[i]));
        cols.Set(kColNames[i], col);
    }
    root.Set(L"columns", cols);
    root.Set(L"sortBy", Value::Str(kColNames[c.sortColumn]));
    root.Set(L"sortDescending", Value::Boolean(c.sortDescending));
    root.Set(L"recentFolders", StrArray(c.recentFolders));
    root.Set(L"hiddenMenuItems", StrArray(c.hiddenMenuItems));
    root.Set(L"seenMenuItems", StrArray(c.seenMenuItems));

    Value w = Value::MakeObject();
    w.Set(L"left", Value::Num(c.windowRect.left));
    w.Set(L"top", Value::Num(c.windowRect.top));
    w.Set(L"right", Value::Num(c.windowRect.right));
    w.Set(L"bottom", Value::Num(c.windowRect.bottom));
    w.Set(L"maximized", Value::Boolean(c.maximized));
    root.Set(L"window", w);
    Value s = Value::MakeObject();
    Value tabs = Value::MakeArray();
    for (auto& t : c.session) {
        Value o = Value::MakeObject();
        o.Set(L"left", Value::Str(t.left));
        o.Set(L"right", Value::Str(t.right));
        o.Set(L"active", Value::Num(t.activePane));
        tabs.arr.push_back(o);
    }
    s.Set(L"tabs", tabs);
    s.Set(L"active", Value::Num(c.sessionActive));
    root.Set(L"session", s);

    Value pins = Value::MakeArray();
    for (auto& p : c.pins) {
        Value o = Value::MakeObject();
        o.Set(L"path", Value::Str(p.path));
        if (!p.name.empty()) o.Set(L"name", Value::Str(p.name));
        if (p.color) o.Set(L"color", Value::Str(HexColor(p.color)));
        pins.arr.push_back(o);
    }
    root.Set(L"pins", pins);
    Value tags = Value::MakeArray();
    for (auto& t : c.tags) {
        Value o = Value::MakeObject();
        o.Set(L"id", Value::Str(t.id));
        o.Set(L"name", Value::Str(t.name));
        o.Set(L"color", Value::Str(HexColor(t.color)));
        tags.arr.push_back(o);
    }
    root.Set(L"tags", tags);
    Value ss = Value::MakeArray();
    for (auto& x : c.savedSearches) {
        Value o = Value::MakeObject();
        o.Set(L"name", Value::Str(x.name));
        o.Set(L"root", Value::Str(x.root));
        o.Set(L"query", Value::Str(x.query));
        ss.arr.push_back(o);
    }
    root.Set(L"savedSearches", ss);
    return root;
}

static std::wstring ToText(const json::Value& v) {
    std::wstring text;
    json::Write(v, text);
    text += L"\n";
    return text;
}

void ConfigLoad() {
    std::wstring text;
    json::Value root;
    if (!ReadUtf8File(ConfigPath(), text)) {
        Log(L"No config.json yet; using defaults");
        DefaultPins();
        ConfigDefaultTags();
        ConfigSave();
        return;
    }
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) {
        Log(L"config.json is invalid; backing it up and using defaults");
        MoveFileExW(ConfigPath().c_str(), (ConfigPath() + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        DefaultPins();
        ConfigDefaultTags();
        ConfigSave();
        return;
    }
    FromJson(root);
    // 0.1.x stored a single-pane layout; Strata is dual-pane by default from 0.2
    if (root.GetNum(L"version", 1) < 2) g_config.layout = L"dual";
    Log(L"Config loaded: %u pins, %u tags, theme %ls, layout %ls", (unsigned)g_config.pins.size(), (unsigned)g_config.tags.size(),
        g_config.theme.c_str(), g_config.layout.c_str());
}

void ConfigSave() {
    if (!WriteUtf8FileAtomic(ConfigPath(), ToText(ToJson()))) Log(L"Failed to save config.json (error %lu)", GetLastError());
}

// ---------------------------------------------------------------- per-folder views
static std::map<std::wstring, FolderView> g_views;
static bool g_viewsLoaded = false;

static void ViewsLoad() {
    g_viewsLoaded = true;
    std::wstring text;
    if (!ReadUtf8File(ViewsPath(), text)) return;
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) return;
    for (auto& kv : root.obj) {
        FolderView v;
        std::wstring sort = kv.second.GetStr(L"sortBy", L"name");
        for (int i = 0; i < ColCount; ++i) if (sort == kColNames[i]) v.sortColumn = i;
        v.sortDescending = kv.second.GetBool(L"desc", false);
        v.viewMode = (int)kv.second.GetNum(L"view", 0);
        g_views[kv.first] = v;
    }
}

static json::Value ViewsJson() {
    json::Value root = json::Value::MakeObject();
    for (auto& kv : g_views) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"sortBy", json::Value::Str(kColNames[kv.second.sortColumn]));
        o.Set(L"desc", json::Value::Boolean(kv.second.sortDescending));
        o.Set(L"view", json::Value::Num(kv.second.viewMode));
        root.Set(kv.first, o);
    }
    return root;
}

bool FolderViewGet(const std::wstring& folder, FolderView& out) {
    if (!g_viewsLoaded) ViewsLoad();
    auto it = g_views.find(ToLower(folder));
    if (it == g_views.end()) return false;
    out = it->second;
    return true;
}

void FolderViewSet(const std::wstring& folder, const FolderView& v) {
    if (!g_viewsLoaded) ViewsLoad();
    g_views[ToLower(folder)] = v;
    if (g_views.size() > 3000) g_views.erase(g_views.begin());
    WriteUtf8FileAtomic(ViewsPath(), ToText(ViewsJson()));
}

// ---------------------------------------------------------------- backup / restore
bool ConfigBackupTo(const std::wstring& path, std::wstring* error) {
    if (!g_viewsLoaded) ViewsLoad();
    json::Value root = json::Value::MakeObject();
    root.Set(L"strataBackup", json::Value::Num(1));
    root.Set(L"version", json::Value::Str(STRATA_VERSION));
    root.Set(L"created", json::Value::Num((double)time(nullptr)));
    root.Set(L"config", ToJson());
    root.Set(L"folderViews", ViewsJson());
    root.Set(L"fileTags", TagsToJson());
    if (!WriteUtf8FileAtomic(path, ToText(root))) { if (error) *error = L"Couldn't write the backup file."; return false; }
    Log(L"Backup written to %ls", path.c_str());
    return true;
}

bool ConfigRestoreFrom(const std::wstring& path, std::wstring* error) {
    std::wstring text;
    if (!ReadUtf8File(path, text)) { if (error) *error = L"Couldn't read that file."; return false; }
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object || !root.Get(L"strataBackup") || !root.Get(L"config")) {
        if (error) *error = L"That file isn't a Strata backup.";
        return false;
    }
    RECT keepRect = g_config.windowRect;
    bool keepMax = g_config.maximized;
    FromJson(*root.Get(L"config"));
    g_config.windowRect = keepRect;
    g_config.maximized = keepMax;
    ConfigSave();
    if (const json::Value* v = root.Get(L"folderViews")) {
        WriteUtf8FileAtomic(ViewsPath(), ToText(*v));
        g_views.clear();
        g_viewsLoaded = false;
    }
    if (const json::Value* t = root.Get(L"fileTags")) TagsFromJson(*t);
    Log(L"Settings restored from %ls", path.c_str());
    return true;
}

// ---------------------------------------------------------------- theme files
// A theme file is a small JSON object using the same keys as the settings:
// {"suiteTheme":1,"name":"...","base":"dark","accentColor":"#4CC2FF","useWindowsAccent":false,
//  "backdrop":"mica","opacity":0.86,"cornerRadius":6,"colors":{"bg":"#202024",...}}
bool ThemeExport(const std::wstring& path, const std::wstring& name) {
    const Config& c = g_config;
    json::Value root = json::Value::MakeObject();
    root.Set(L"suiteTheme", json::Value::Num(1));
    root.Set(L"name", json::Value::Str(name));
    root.Set(L"base", json::Value::Str(c.theme == L"custom" ? L"dark" : c.theme));
    root.Set(L"useWindowsAccent", json::Value::Boolean(c.useWindowsAccent));
    root.Set(L"accentColor", json::Value::Str(HexColor(c.accentColor)));
    root.Set(L"backdrop", json::Value::Str(c.backdrop));
    root.Set(L"opacity", json::Value::Num(std::round(c.opacity * 100) / 100));
    root.Set(L"cornerRadius", json::Value::Num(std::round(c.cornerRadius)));
    json::Value colors = json::Value::MakeObject();
    for (auto& kv : c.customColors) colors.Set(kv.first, json::Value::Str(HexColor(kv.second)));
    root.Set(L"colors", colors);
    return WriteUtf8FileAtomic(path, ToText(root));
}

bool ThemeImport(const std::wstring& path, std::wstring* error) {
    std::wstring text;
    if (!ReadUtf8File(path, text)) { if (error) *error = L"Couldn't read that file."; return false; }
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) { if (error) *error = L"That isn't a theme file."; return false; }
    Config& c = g_config;
    std::wstring base = root.GetStr(L"base", root.GetStr(L"theme", L"dark"));
    c.useWindowsAccent = root.GetBool(L"useWindowsAccent", false);
    c.accentColor = ParseHexColor(root.GetStr(L"accentColor"), c.accentColor);
    c.backdrop = OneOf(root.GetStr(L"backdrop", c.backdrop), {L"solid", L"mica", L"acrylic"}, L"mica");
    c.opacity = std::clamp((float)root.GetNum(L"opacity", c.opacity), 0.4f, 1.0f);
    c.cornerRadius = std::clamp((float)root.GetNum(L"cornerRadius", c.cornerRadius), 0.0f, 16.0f);
    c.customColors.clear();
    if (const json::Value* cc = root.Get(L"colors"); cc && cc->type == json::Value::Object)
        for (auto& kv : cc->obj) if (kv.second.type == json::Value::String) c.customColors.push_back({kv.first, ParseHexColor(kv.second.s, 0)});
    std::wstring baseOk = OneOf(base, {L"dark", L"light", L"midnight", L"glass", L"ember", L"ocean", L"system"}, L"dark");
    if (c.customColors.empty()) {
        c.theme = baseOk;
    } else {
        c.theme = L"custom";
        c.customTheme = root.GetStr(L"name", L"Imported theme");
        c.customColors.insert(c.customColors.begin(), {L"base:" + baseOk, 0});
    }
    ConfigSave();
    return true;
}
