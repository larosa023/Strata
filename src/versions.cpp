// Strata - version stacking (see versions.h)
#include "versions.h"
#include <cwctype>

static bool IsSep(wchar_t c) { return c == L' ' || c == L'_' || c == L'-' || c == L'.'; }
static void TrimSeps(std::wstring& s) {
    while (!s.empty() && IsSep(s.back())) s.pop_back();
    while (!s.empty() && IsSep(s.front())) s.erase(s.begin());
}
static bool AllDigits(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) if (!iswdigit(c)) return false;
    return true;
}
static bool IsRevWord(const std::wstring& w) {
    return w == L"rev" || w == L"r" || w == L"v" || w == L"ver" || w == L"version" || w == L"revision" || w == L"rv";
}
// "rev3" / "v12" / "reva" -> rank; false if not a glued revision token
static bool GluedRev(const std::wstring& t, double& rank) {
    static const wchar_t* words[] = {L"revision", L"version", L"rev", L"ver", L"rv", L"r", L"v"};
    for (const wchar_t* w : words) {
        size_t n = wcslen(w);
        if (t.size() <= n || t.compare(0, n, w) != 0) continue;
        std::wstring rest = t.substr(n);
        if (!rest.empty() && rest[0] == L'.') rest.erase(0, 1);
        if (AllDigits(rest) && rest.size() <= 3) { rank = _wtoi(rest.c_str()); return true; }
        bool letterOk = n >= 3;   // "reva" / "revb" but not "va" / "ra"
        if (letterOk && rest.size() == 1 && iswalpha(rest[0])) { rank = 1 + (rest[0] - L'a'); return true; }
        return false;
    }
    return false;
}
static bool DateToken(const std::wstring& t, double& rank) {
    std::wstring d;
    for (wchar_t c : t) if (iswdigit(c)) d += c; else if (c != L'-' && c != L'_' && c != L'.') return false;
    if (d.size() != 8 || !(d.compare(0, 2, L"19") == 0 || d.compare(0, 2, L"20") == 0)) return false;
    int m = _wtoi(d.substr(4, 2).c_str()), day = _wtoi(d.substr(6, 2).c_str());
    if (m < 1 || m > 12 || day < 1 || day > 31) return false;
    rank = _wtof(d.c_str());
    return true;
}

static bool Strip(std::wstring base, std::wstring& stem, double& rank, std::wstring* label) {
    bool marker = false;
    rank = -1;
    TrimSeps(base);
    // "Plans (2)"
    if (!base.empty() && base.back() == L')') {
        size_t open = base.find_last_of(L'(');
        if (open != std::wstring::npos) {
            std::wstring inner = base.substr(open + 1, base.size() - open - 2);
            if (AllDigits(inner) && inner.size() <= 3) {
                rank = _wtoi(inner.c_str());
                if (label) *label = L"(" + inner + L")";
                base = base.substr(0, open);
                TrimSeps(base);
                marker = true;
            }
        }
    }
    // "Plans - Copy"
    for (const wchar_t* c : {L"copy"}) {
        size_t n = wcslen(c);
        if (base.size() > n + 1 && base.compare(base.size() - n, n, c) == 0 && IsSep(base[base.size() - n - 1])) {
            base = base.substr(0, base.size() - n);
            TrimSeps(base);
            if (!marker) { rank = 0; if (label) *label = L"copy"; }
            marker = true;
        }
    }
    // a date at the end: "Plans 2026-03-01" / "Plans_20260301"
    if (base.size() > 10) {
        std::wstring tail10 = base.substr(base.size() - 10);
        double r;
        if (IsSep(base[base.size() - 11]) && DateToken(tail10, r)) {
            std::wstring head = base.substr(0, base.size() - 10);
            TrimSeps(head);
            if (!head.empty()) {
                stem = head;
                rank = r;
                if (label) *label = tail10;
                return true;
            }
        }
    }
    size_t sep = std::wstring::npos;
    for (size_t i = base.size(); i-- > 0;) if (IsSep(base[i])) { sep = i; break; }
    if (sep != std::wstring::npos) {
        std::wstring tail = base.substr(sep + 1), head = base.substr(0, sep);
        TrimSeps(head);
        double r;
        if (!head.empty() && DateToken(tail, r)) {
            stem = head; rank = r; if (label) *label = tail;
            return true;
        }
        if (!head.empty() && GluedRev(tail, r)) {
            stem = head; rank = r; if (label) *label = tail;
            return true;
        }
        // "Plans rev 3" / "Plans v 2" / "Plans Rev B"
        if (!head.empty() && ((AllDigits(tail) && tail.size() <= 3) || (tail.size() == 1 && iswalpha(tail[0])))) {
            size_t sep2 = std::wstring::npos;
            for (size_t i = head.size(); i-- > 0;) if (IsSep(head[i])) { sep2 = i; break; }
            std::wstring word = sep2 == std::wstring::npos ? head : head.substr(sep2 + 1);
            std::wstring head2 = sep2 == std::wstring::npos ? L"" : head.substr(0, sep2);
            TrimSeps(head2);
            bool letter = !AllDigits(tail);
            if (IsRevWord(word) && !head2.empty() && (!letter || word.size() >= 3)) {
                stem = head2;
                rank = letter ? 1 + (tail[0] - L'a') : _wtoi(tail.c_str());
                if (label) *label = word + L" " + tail;
                return true;
            }
        }
    }
    stem = base;
    return marker;
}

static void SplitName(const std::wstring& name, bool isDir, std::wstring& base, std::wstring& ext) {
    std::wstring lower = ToLower(name);
    size_t dot = isDir ? std::wstring::npos : lower.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) { base = lower; ext.clear(); }
    else { base = lower.substr(0, dot); ext = lower.substr(dot); }
}

VersionKey ParseVersion(const std::wstring& name, bool isDir) {
    VersionKey v;
    std::wstring base, ext, stem;
    SplitName(name, isDir, base, ext);
    double rank = -1;
    v.marker = Strip(base, stem, rank, nullptr);
    if (stem.empty()) { stem = base; v.marker = false; rank = -1; }
    v.rank = v.marker ? rank : -1;
    v.key = stem + L"|" + ext + (isDir ? L"/" : L"");
    return v;
}

std::wstring VersionLabel(const std::wstring& name, bool isDir) {
    std::wstring base, ext, stem, label;
    SplitName(name, isDir, base, ext);
    double rank;
    if (!Strip(base, stem, rank, &label)) return L"";
    // show it as written in the file name ("Rev B", not "rev b")
    std::wstring lowerName = ToLower(name);
    size_t at = lowerName.rfind(label);
    return at == std::wstring::npos ? label : name.substr(at, label.size());
}
