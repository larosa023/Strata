// Strata - folder visit history (visits.json)
#include "visits.h"
#include "config.h"
#include "json.h"
#include "fs.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <unordered_map>

namespace {
struct Visit {
    std::wstring path;   // as first seen (display case)
    uint32_t count = 0;
    uint64_t last = 0;   // unix seconds
    uint64_t prev = 0;   // the visit before `last` (a separate session: > 30 minutes earlier)
};
std::unordered_map<std::wstring, Visit> g_visits;   // key: lowercase path
bool g_loaded = false, g_dirty = false;
uint64_t g_lastSave = 0;
}  // namespace

static uint64_t NowUnix() { return (uint64_t)time(nullptr); }
static std::wstring VisitsPath() { return DataDir() + L"\\visits.json"; }

static void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::wstring text;
    if (!ReadUtf8File(VisitsPath(), text)) return;
    json::Value root;
    if (!json::Parser(text).Parse(root) || root.type != json::Value::Array) return;
    for (auto& v : root.arr) {
        if (v.type != json::Value::Object) continue;
        Visit x;
        x.path = v.GetStr(L"p");
        if (x.path.empty()) continue;
        x.count = (uint32_t)v.GetNum(L"n", 1);
        x.last = (uint64_t)v.GetNum(L"t", 0);
        x.prev = (uint64_t)v.GetNum(L"pt", 0);
        g_visits[ToLower(x.path)] = x;
    }
}

static float Frecency(const Visit& v, uint64_t now) {
    double age = now > v.last ? (double)(now - v.last) : 0;
    double rec = age < 3600 ? 4.0 : age < 86400 ? 2.0 : age < 7 * 86400 ? 1.4 : age < 30 * 86400 ? 1.0 : 0.5;
    return (float)(std::log2(1.0 + v.count) * rec);
}

void VisitsSave(bool force) {
    if (!g_loaded || !g_dirty) return;
    uint64_t now = NowUnix();
    if (!force && now - g_lastSave < 30) return;
    // keep the 3000 most useful
    std::vector<const Visit*> all;
    for (auto& kv : g_visits) all.push_back(&kv.second);
    if (all.size() > 3000) {
        std::sort(all.begin(), all.end(), [&](const Visit* a, const Visit* b) { return Frecency(*a, now) > Frecency(*b, now); });
        all.resize(3000);
    }
    json::Value root = json::Value::MakeArray();
    for (auto* v : all) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"p", json::Value::Str(v->path));
        o.Set(L"n", json::Value::Num(v->count));
        o.Set(L"t", json::Value::Num((double)v->last));
        if (v->prev) o.Set(L"pt", json::Value::Num((double)v->prev));
        root.arr.push_back(o);
    }
    std::wstring out;
    json::Write(root, out);
    WriteUtf8FileAtomic(VisitsPath(), out);
    g_dirty = false;
    g_lastSave = now;
}

void VisitRecord(const std::wstring& folderIn) {
    std::wstring folder = NormalizeFolder(folderIn);
    if (folder.empty()) return;
    Load();
    uint64_t now = NowUnix();
    Visit& v = g_visits[ToLower(folder)];
    if (v.path.empty()) v.path = folder;
    if (v.last && now - v.last > 30 * 60) v.prev = v.last;
    if (!v.last || now - v.last > 60) ++v.count;   // revisits within a minute don't count twice
    v.last = now;
    g_dirty = true;
    VisitsSave(false);
}

bool VisitPrevious(const std::wstring& folder, uint64_t& unixTime) {
    Load();
    auto it = g_visits.find(ToLower(NormalizeFolder(folder)));
    if (it == g_visits.end() || !it->second.prev) return false;
    unixTime = it->second.prev;
    return true;
}

void VisitForget(const std::wstring& folder) {
    Load();
    g_visits.erase(ToLower(NormalizeFolder(folder)));
    g_dirty = true;
}

static std::vector<std::wstring> Tokens(const std::wstring& q) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : ToLower(q)) {
        if (c == L' ' || c == L'\\' || c == L'/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool WordStart(const std::wstring& s, size_t i) {
    if (i == 0) return true;
    wchar_t p = s[i - 1];
    return p == L'\\' || p == L' ' || p == L'_' || p == L'-' || p == L'.' || p == L'(' || (iswlower(p) && iswupper(s[i]));
}

// 0 = no match. Every token must appear; matches at word starts, in the folder's own name, and in order score higher.
static float MatchScore(const std::wstring& path, const std::wstring& lower, const std::vector<std::wstring>& toks) {
    if (toks.empty()) return 1;
    size_t leafStart = lower.find_last_of(L'\\');
    leafStart = leafStart == std::wstring::npos || leafStart + 1 >= lower.size() ? 0 : leafStart + 1;
    float score = 0;
    size_t lastPos = 0;
    bool inOrder = true;
    for (size_t k = 0; k < toks.size(); ++k) {
        const std::wstring& t = toks[k];
        size_t best = std::wstring::npos;
        float bestS = 0;
        for (size_t p = lower.find(t); p != std::wstring::npos; p = lower.find(t, p + 1)) {
            float s = 1;
            if (WordStart(path, p)) s += 2;
            if (p >= leafStart) s += k + 1 == toks.size() ? 4 : 1.5f;
            if (p == leafStart && t.size() + leafStart == lower.size()) s += 3;   // whole folder name
            if (p >= lastPos) s += 0.5f;
            if (s > bestS) { bestS = s; best = p; }
        }
        if (best == std::wstring::npos) return 0;
        if (best < lastPos) inOrder = false;
        lastPos = best + t.size();
        score += bestS * (0.6f + std::min(1.0f, t.size() / 4.0f));
    }
    if (inOrder) score *= 1.25f;
    // shorter paths win ties
    return score / (1.0f + lower.size() / 400.0f);
}

std::vector<JumpHit> JumpQuery(const std::wstring& query, const std::vector<std::wstring>& pins, const std::vector<std::wstring>& extraIn, size_t max) {
    std::vector<std::wstring> extra = pins;
    extra.insert(extra.end(), extraIn.begin(), extraIn.end());
    Load();
    auto toks = Tokens(query);
    uint64_t now = NowUnix();
    std::unordered_map<std::wstring, JumpHit> hits;
    for (auto& kv : g_visits) {
        float m = MatchScore(kv.second.path, kv.first, toks);
        if (m <= 0) continue;
        hits[kv.first] = {kv.second.path, m * (1.0f + Frecency(kv.second, now)), false};
    }
    for (size_t i = 0; i < extra.size(); ++i) {
        const std::wstring& e = extra[i];
        bool pin = i < pins.size();
        std::wstring lower = ToLower(e);
        float m = MatchScore(e, lower, toks);
        if (m <= 0) continue;
        auto it = hits.find(lower);
        if (it == hits.end()) hits[lower] = {e, m * (pin ? 2.0f : 1.2f), pin};
        else if (pin && !it->second.pinned) { it->second.score *= 1.3f; it->second.pinned = true; }
    }
    std::vector<JumpHit> out;
    for (auto& kv : hits) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const JumpHit& a, const JumpHit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.path.size() < b.path.size();
    });
    // drop folders that are gone (local paths only: network paths could stall)
    std::vector<JumpHit> kept;
    for (auto& h : out) {
        if (kept.size() >= max) break;
        bool unc = h.path.size() > 2 && h.path[0] == L'\\' && h.path[1] == L'\\';
        if (!unc && GetFileAttributesW(h.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            g_visits.erase(ToLower(h.path));
            g_dirty = true;
            continue;
        }
        kept.push_back(h);
    }
    return kept;
}

std::vector<std::pair<size_t, size_t>> JumpHighlights(const std::wstring& path, const std::wstring& query) {
    std::vector<std::pair<size_t, size_t>> out;
    std::wstring lower = ToLower(path);
    size_t leafStart = lower.find_last_of(L'\\');
    leafStart = leafStart == std::wstring::npos ? 0 : leafStart + 1;
    for (auto& t : Tokens(query)) {
        // prefer the occurrence in the folder name, then at a word start
        size_t pick = std::wstring::npos;
        for (size_t p = lower.find(t); p != std::wstring::npos; p = lower.find(t, p + 1)) {
            if (pick == std::wstring::npos) pick = p;
            if (p >= leafStart) { pick = p; break; }
            if (WordStart(path, p) && !WordStart(path, pick)) pick = p;
        }
        if (pick != std::wstring::npos) out.push_back({pick, t.size()});
    }
    return out;
}
