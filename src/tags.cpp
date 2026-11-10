// Strata - color tags on files and folders
#include "tags.h"
#include "config.h"
#include <map>

static std::map<std::wstring, std::pair<std::wstring, std::wstring>> g_tags;   // lowercase path -> (tag id, original path)
static bool g_loaded = false;

static std::wstring TagsPath() { return DataDir() + L"\\tags.json"; }

json::Value TagsToJson() {
    json::Value root = json::Value::MakeObject();
    for (auto& kv : g_tags) root.Set(kv.second.second, json::Value::Str(kv.second.first));
    return root;
}

static void Save() {
    std::wstring text;
    json::Write(TagsToJson(), text);
    WriteUtf8FileAtomic(TagsPath(), text + L"\n");
}

void TagsFromJson(const json::Value& v) {
    g_tags.clear();
    g_loaded = true;
    if (v.type != json::Value::Object) return;
    for (auto& kv : v.obj)
        if (kv.second.type == json::Value::String && !kv.second.s.empty()) g_tags[ToLower(kv.first)] = {kv.second.s, kv.first};
    Save();
}

void TagsLoad() {
    if (g_loaded) return;
    g_loaded = true;
    std::wstring text;
    if (!ReadUtf8File(TagsPath(), text)) return;
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) return;
    for (auto& kv : root.obj)
        if (kv.second.type == json::Value::String && !kv.second.s.empty()) g_tags[ToLower(kv.first)] = {kv.second.s, kv.first};
}

std::wstring TagOf(const std::wstring& path) {
    if (!g_loaded) TagsLoad();
    if (g_tags.empty()) return L"";
    auto it = g_tags.find(ToLower(path));
    return it == g_tags.end() ? L"" : it->second.first;
}

void TagSet(const std::wstring& path, const std::wstring& tagId) {
    if (!g_loaded) TagsLoad();
    if (tagId.empty()) g_tags.erase(ToLower(path));
    else g_tags[ToLower(path)] = {tagId, path};
    Save();
}

void TagsPathMoved(const std::wstring& from, const std::wstring& to) {
    if (!g_loaded) TagsLoad();
    std::wstring f = ToLower(from);
    std::vector<std::pair<std::wstring, std::pair<std::wstring, std::wstring>>> moved;
    for (auto it = g_tags.begin(); it != g_tags.end();) {
        const std::wstring& k = it->first;
        if (k == f || (k.size() > f.size() && k.compare(0, f.size(), f) == 0 && k[f.size()] == L'\\')) {
            std::wstring np = to + it->second.second.substr(from.size());
            moved.push_back({ToLower(np), {it->second.first, np}});
            it = g_tags.erase(it);
        } else {
            ++it;
        }
    }
    if (moved.empty()) return;
    for (auto& m : moved) g_tags[m.first] = m.second;
    Save();
}

std::vector<std::wstring> TagsFind(const std::wstring& tagId) {
    if (!g_loaded) TagsLoad();
    std::vector<std::wstring> out;
    for (auto& kv : g_tags) if (kv.second.first == tagId) out.push_back(kv.second.second);
    return out;
}

void TagsRemoveTag(const std::wstring& tagId) {
    if (!g_loaded) TagsLoad();
    for (auto it = g_tags.begin(); it != g_tags.end();) it = it->second.first == tagId ? g_tags.erase(it) : std::next(it);
    Save();
}

const TagDef* TagDefById(const std::wstring& id) {
    for (auto& t : g_config.tags) if (t.id == id) return &t;
    return nullptr;
}

uint32_t TagColor(const std::wstring& id) {
    if (id.empty()) return 0;
    const TagDef* d = TagDefById(id);
    return d ? d->color : 0;
}
