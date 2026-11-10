// Strata - search query parsing
#include "query.h"
#include <shlwapi.h>
#include <ctime>

static uint64_t FtToU(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static uint64_t ParseSize(std::wstring s) {
    double mult = 1;
    std::wstring l = ToLower(s);
    auto ends = [&](const wchar_t* suf) { size_t n = wcslen(suf); return l.size() > n && l.compare(l.size() - n, n, suf) == 0; };
    if (ends(L"kb") || ends(L"k")) mult = 1024.0;
    else if (ends(L"mb") || ends(L"m")) mult = 1024.0 * 1024;
    else if (ends(L"gb") || ends(L"g")) mult = 1024.0 * 1024 * 1024;
    else if (ends(L"tb") || ends(L"t")) mult = 1024.0 * 1024 * 1024 * 1024;
    return (uint64_t)(_wtof(l.c_str()) * mult);
}

static uint64_t NowFt() { FILETIME ft; GetSystemTimeAsFileTime(&ft); return FtToU(ft); }

static uint64_t YearFt(int year, int month = 1, int day = 1) {
    SYSTEMTIME st{};
    st.wYear = (WORD)year;
    st.wMonth = (WORD)month;
    st.wDay = (WORD)day;
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    return FtToU(ft);
}

SearchQuery ParseQuery(const std::wstring& text) {
    SearchQuery q;
    std::vector<std::wstring> toks;
    std::wstring cur;
    bool quoted = false;
    for (wchar_t c : text) {
        if (c == L'"') { quoted = !quoted; continue; }
        if (!quoted && (c == L' ' || c == L'\t')) { if (!cur.empty()) toks.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);
    for (auto& tok : toks) {
        std::wstring t = ToLower(tok);
        size_t colon = t.find(L':');
        std::wstring key = colon == std::wstring::npos ? L"" : t.substr(0, colon), val = colon == std::wstring::npos ? L"" : t.substr(colon + 1);
        if (key == L"type" || key == L"ext") {
            size_t i = 0;
            while (i <= val.size()) {
                size_t c = val.find_first_of(L",;", i);
                std::wstring e = val.substr(i, c == std::wstring::npos ? std::wstring::npos : c - i);
                if (!e.empty() && e[0] == L'.') e.erase(e.begin());
                if (!e.empty()) q.exts.push_back(e);
                if (c == std::wstring::npos) break;
                i = c + 1;
            }
        } else if (key == L"size") {
            if (val.rfind(L">=", 0) == 0) q.minSize = ParseSize(val.substr(2));
            else if (val.rfind(L"<=", 0) == 0) q.maxSize = ParseSize(val.substr(2));
            else if (!val.empty() && val[0] == L'>') q.minSize = ParseSize(val.substr(1)) + 1;
            else if (!val.empty() && val[0] == L'<') q.maxSize = ParseSize(val.substr(1));
            else if (!val.empty()) { uint64_t s = ParseSize(val); q.minSize = s * 9 / 10; q.maxSize = s * 11 / 10; }
        } else if (key == L"since") {   // since:<unix seconds> (used by "What changed")
            uint64_t secs = (uint64_t)_wtoi64(val.c_str());
            if (secs) q.modifiedAfter = secs * 10000000ULL + 116444736000000000ULL;
        } else if (key == L"modified" || key == L"dm" || key == L"date") {
            const uint64_t day = 864000000000ull;
            if (val == L"today") q.modifiedAfter = NowFt() - day;
            else if (val.size() >= 2 && (val.back() == L'd' || val.back() == L'w' || val.back() == L'm' || val.back() == L'y') && iswdigit(val[0])) {
                uint64_t n = _wtoi(val.c_str());
                uint64_t span = val.back() == L'd' ? day : val.back() == L'w' ? 7 * day : val.back() == L'm' ? 30 * day : 365 * day;
                q.modifiedAfter = NowFt() - n * span;
            } else if (val.size() == 4 && iswdigit(val[0])) {
                int y = _wtoi(val.c_str());
                q.modifiedAfter = YearFt(y);
                q.modifiedBefore = YearFt(y + 1);
            } else if (val.size() == 7 && val[4] == L'-') {
                int y = _wtoi(val.c_str()), m = _wtoi(val.c_str() + 5);
                q.modifiedAfter = YearFt(y, m);
                q.modifiedBefore = m == 12 ? YearFt(y + 1) : YearFt(y, m + 1);
            }
        } else if (key == L"tag") {
            q.tag = val;
        } else if (t == L"folder:" || t == L"folders:") {
            q.foldersOnly = true;
        } else if (t == L"file:" || t == L"files:") {
            q.filesOnly = true;
        } else {
            q.words.push_back(t);
        }
    }
    return q;
}

bool SearchQuery::Match(const std::wstring& nameLower, bool isDir, uint64_t size, const FILETIME& modified) const {
    if (foldersOnly && !isDir) return false;
    if (filesOnly && isDir) return false;
    for (auto& w : words) {
        if (w.find_first_of(L"*?") != std::wstring::npos) {
            if (!PathMatchSpecW(nameLower.c_str(), w.c_str())) return false;
        } else if (nameLower.find(w) == std::wstring::npos) {
            return false;
        }
    }
    if (!exts.empty()) {
        if (isDir) return false;
        size_t dot = nameLower.find_last_of(L'.');
        if (dot == std::wstring::npos) return false;
        std::wstring e = nameLower.substr(dot + 1);
        bool any = false;
        for (auto& x : exts) if (e == x) any = true;
        if (!any) return false;
    }
    if (!isDir && (size < minSize || size > maxSize)) return false;
    if (isDir && (minSize > 0 || maxSize != UINT64_MAX)) return false;
    if (modifiedAfter || modifiedBefore != UINT64_MAX) {
        uint64_t m = FtToU(modified);
        if (m < modifiedAfter || m >= modifiedBefore) return false;
    }
    return true;
}

static std::wstring FtDate(uint64_t ft) {
    FILETIME f{(DWORD)(ft & 0xFFFFFFFF), (DWORD)(ft >> 32)};
    SYSTEMTIME st;
    FileTimeToSystemTime(&f, &st);
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

std::wstring SearchQuery::ToEverything() const {
    std::wstring s;
    for (auto& w : words) s += (s.empty() ? L"" : L" ") + (w.find(L' ') != std::wstring::npos ? L"\"" + w + L"\"" : w);
    if (!exts.empty()) {
        s += L" ext:";
        for (size_t i = 0; i < exts.size(); ++i) s += (i ? L";" : L"") + exts[i];
    }
    if (minSize > 0) s += L" size:>=" + std::to_wstring(minSize);
    if (maxSize != UINT64_MAX) s += L" size:<=" + std::to_wstring(maxSize);
    if (modifiedAfter) s += L" dm:>=" + FtDate(modifiedAfter);
    if (modifiedBefore != UINT64_MAX) s += L" dm:<" + FtDate(modifiedBefore);
    if (foldersOnly) s += L" folder:";
    if (filesOnly) s += L" file:";
    return s;
}
