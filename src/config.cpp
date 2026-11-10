// Strata - settings, pins and session persisted as config.json
#include "config.h"
#include "json.h"
#include <shlobj.h>
#include <algorithm>
#include <cmath>

Config g_config;

std::wstring ConfigPath() { return DataDir() + L"\\config.json"; }

bool ReadUtf8File(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size > 8 * 1024 * 1024) { CloseHandle(h); return false; }
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

static uint32_t ParseHexColor(const std::wstring& s, uint32_t def) {
    std::wstring t = s;
    if (!t.empty() && t[0] == L'#') t = t.substr(1);
    if (t.size() != 6) return def;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(t.c_str(), &end, 16);
    return (end && *end == 0) ? (uint32_t)v : def;
}

static std::wstring HexColor(uint32_t c) {
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

static const wchar_t* kColNames[ColCount] = {L"name", L"size", L"modified", L"type"};

static void FromJson(const json::Value& root) {
    Config d;
    Config& c = g_config;
    c.theme = OneOf(root.GetStr(L"theme", d.theme), {L"dark", L"light", L"midnight", L"glass", L"ember", L"ocean", L"system"}, L"dark");
    c.useWindowsAccent = root.GetBool(L"useWindowsAccent", d.useWindowsAccent);
    c.accentColor = ParseHexColor(root.GetStr(L"accentColor"), d.accentColor);
    c.backdrop = OneOf(root.GetStr(L"backdrop", d.backdrop), {L"solid", L"mica", L"acrylic"}, L"mica");
    c.opacity = std::clamp((float)root.GetNum(L"opacity", d.opacity), 0.4f, 1.0f);
    c.cornerRadius = std::clamp((float)root.GetNum(L"cornerRadius", d.cornerRadius), 0.0f, 16.0f);
    c.density = OneOf(root.GetStr(L"density", d.density), {L"compact", L"comfortable"}, L"comfortable");

    c.showHidden = root.GetBool(L"showHidden", d.showHidden);
    c.showExtensions = root.GetBool(L"showExtensions", d.showExtensions);
    c.foldersFirst = root.GetBool(L"foldersFirst", d.foldersFirst);
    if (const json::Value* cols = root.Get(L"columns"); cols && cols->type == json::Value::Object) {
        for (int i = 0; i < ColCount; ++i) {
            const json::Value* col = cols->Get(kColNames[i]);
            if (!col) continue;
            c.columnWidth[i] = std::clamp((float)col->GetNum(L"width", d.columnWidth[i]), 48.0f, 2000.0f);
            c.columnVisible[i] = i == ColName ? true : col->GetBool(L"visible", true);
        }
    }
    std::wstring sort = root.GetStr(L"sortBy", L"name");
    for (int i = 0; i < ColCount; ++i) if (sort == kColNames[i]) c.sortColumn = i;
    c.sortDescending = root.GetBool(L"sortDescending", false);
    c.sidebarWidth = std::clamp((float)root.GetNum(L"sidebarWidth", d.sidebarWidth), 150.0f, 480.0f);

    if (const json::Value* w = root.Get(L"window"); w && w->type == json::Value::Object) {
        c.windowRect = {(LONG)w->GetNum(L"left", 0), (LONG)w->GetNum(L"top", 0), (LONG)w->GetNum(L"right", 0),
                        (LONG)w->GetNum(L"bottom", 0)};
        c.maximized = w->GetBool(L"maximized", false);
    }
    c.restoreSession = root.GetBool(L"restoreSession", true);
    c.sessionTabs.clear();
    if (const json::Value* s = root.Get(L"session"); s && s->type == json::Value::Object) {
        if (const json::Value* tabs = s->Get(L"tabs"); tabs && tabs->type == json::Value::Array)
            for (auto& v : tabs->arr) if (v.type == json::Value::String) c.sessionTabs.push_back(v.s);
        c.sessionActive = (int)s->GetNum(L"active", 0);
    }

    c.pins.clear();
    if (const json::Value* pins = root.Get(L"pins"); pins && pins->type == json::Value::Array) {
        for (auto& v : pins->arr) {
            Pin pin{v.GetStr(L"path"), v.GetStr(L"name"), ParseHexColor(v.GetStr(L"color"), 0)};
            if (!pin.path.empty()) c.pins.push_back(pin);
        }
    }
}

static std::wstring ToJsonText() {
    const Config& c = g_config;
    using json::Value;
    Value root = Value::MakeObject();
    root.Set(L"version", Value::Num(1));
    root.Set(L"theme", Value::Str(c.theme));
    root.Set(L"useWindowsAccent", Value::Boolean(c.useWindowsAccent));
    root.Set(L"accentColor", Value::Str(HexColor(c.accentColor)));
    root.Set(L"backdrop", Value::Str(c.backdrop));
    root.Set(L"opacity", Value::Num(std::round(c.opacity * 100) / 100));
    root.Set(L"cornerRadius", Value::Num(std::round(c.cornerRadius)));
    root.Set(L"density", Value::Str(c.density));

    root.Set(L"showHidden", Value::Boolean(c.showHidden));
    root.Set(L"showExtensions", Value::Boolean(c.showExtensions));
    root.Set(L"foldersFirst", Value::Boolean(c.foldersFirst));
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
    root.Set(L"sidebarWidth", Value::Num(std::round(c.sidebarWidth)));

    Value w = Value::MakeObject();
    w.Set(L"left", Value::Num(c.windowRect.left));
    w.Set(L"top", Value::Num(c.windowRect.top));
    w.Set(L"right", Value::Num(c.windowRect.right));
    w.Set(L"bottom", Value::Num(c.windowRect.bottom));
    w.Set(L"maximized", Value::Boolean(c.maximized));
    root.Set(L"window", w);
    root.Set(L"restoreSession", Value::Boolean(c.restoreSession));
    Value s = Value::MakeObject();
    Value tabs = Value::MakeArray();
    for (auto& t : c.sessionTabs) tabs.arr.push_back(Value::Str(t));
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
    std::wstring text;
    json::Write(root, text);
    text += L"\n";
    return text;
}

void ConfigLoad() {
    std::wstring text;
    json::Value root;
    if (!ReadUtf8File(ConfigPath(), text)) {
        Log(L"No config.json yet; using defaults");
        DefaultPins();
        ConfigSave();
        return;
    }
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) {
        Log(L"config.json is invalid; backing it up and using defaults");
        MoveFileExW(ConfigPath().c_str(), (ConfigPath() + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        DefaultPins();
        ConfigSave();
        return;
    }
    FromJson(root);
    Log(L"Config loaded: %u pins, theme %ls", (unsigned)g_config.pins.size(), g_config.theme.c_str());
}

void ConfigSave() {
    if (!WriteUtf8FileAtomic(ConfigPath(), ToJsonText())) Log(L"Failed to save config.json (error %lu)", GetLastError());
}
