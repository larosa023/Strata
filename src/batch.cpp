// Strata - batch rename with live preview
#include "batch.h"
#include "fs.h"
#include "dialogs.h"
#include <commctrl.h>
#include <regex>
#include <set>

namespace {
struct BatchState {
    std::vector<std::wstring> paths;
    std::vector<std::wstring> newNames;
    std::vector<std::pair<std::wstring, std::wstring>> result;
    std::wstring problem;
};

enum : WORD { IdFind = 101, IdReplace, IdRegex, IdCase, IdPrefix, IdSuffix, IdNumber, IdStart, IdCaseMode, IdList, IdStatus, IdExt };
}  // namespace

static std::wstring GetText(HWND dlg, int id) {
    wchar_t buf[1024] = {};
    GetDlgItemTextW(dlg, id, buf, 1024);
    return buf;
}

static std::wstring ApplyCase(const std::wstring& s, int mode) {
    std::wstring r = s;
    if (mode == 1) CharLowerBuffW(&r[0], (DWORD)r.size());
    else if (mode == 2) CharUpperBuffW(&r[0], (DWORD)r.size());
    else if (mode == 3) {   // Title Case
        bool start = true;
        for (auto& c : r) {
            if (iswalpha(c)) { c = start ? towupper(c) : towlower(c); start = false; }
            else start = c == L' ' || c == L'_' || c == L'-' || c == L'.';
        }
    }
    return r;
}

static void Recompute(HWND dlg, BatchState* st) {
    std::wstring find = GetText(dlg, IdFind), repl = GetText(dlg, IdReplace);
    std::wstring prefix = GetText(dlg, IdPrefix), suffix = GetText(dlg, IdSuffix);
    bool regex = IsDlgButtonChecked(dlg, IdRegex) == BST_CHECKED;
    bool matchCase = IsDlgButtonChecked(dlg, IdCase) == BST_CHECKED;
    bool number = IsDlgButtonChecked(dlg, IdNumber) == BST_CHECKED;
    bool keepExt = IsDlgButtonChecked(dlg, IdExt) == BST_CHECKED;
    int start = _wtoi(GetText(dlg, IdStart).c_str());
    int caseMode = (int)SendDlgItemMessageW(dlg, IdCaseMode, CB_GETCURSEL, 0, 0);
    st->newNames.clear();
    st->problem.clear();
    std::wregex re;
    bool reOk = true;
    if (regex && !find.empty()) {
        try { re = std::wregex(find, matchCase ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase)); }
        catch (...) { reOk = false; st->problem = L"That regular expression isn't valid."; }
    }
    int digits = std::max(2, (int)std::to_wstring(start + (int)st->paths.size()).size());
    for (size_t i = 0; i < st->paths.size(); ++i) {
        std::wstring leaf = PathLeaf(st->paths[i]);
        std::wstring base = leaf, ext;
        size_t dot = leaf.find_last_of(L'.');
        if (keepExt && dot != std::wstring::npos && dot > 0 && !IsDirectory(st->paths[i])) { base = leaf.substr(0, dot); ext = leaf.substr(dot); }
        if (!find.empty()) {
            if (regex && reOk) {
                try { base = std::regex_replace(base, re, repl); } catch (...) {}
            } else if (!regex) {
                std::wstring hay = matchCase ? base : ToLower(base), needle = matchCase ? find : ToLower(find);
                std::wstring out;
                size_t pos = 0;
                for (;;) {
                    size_t k = hay.find(needle, pos);
                    if (k == std::wstring::npos) { out += base.substr(pos); break; }
                    out += base.substr(pos, k - pos) + repl;
                    pos = k + needle.size();
                }
                base = out;
            }
        }
        base = ApplyCase(base, caseMode);
        std::wstring num;
        if (number) {
            num = std::to_wstring(start + (int)i);
            while ((int)num.size() < digits) num = L"0" + num;
        }
        // "#" in the prefix/suffix is replaced by the number; otherwise the number goes at the end
        auto sub = [&](std::wstring s) { size_t h = s.find(L'#'); if (h != std::wstring::npos && number) s.replace(h, 1, num); return s; };
        std::wstring name = sub(prefix) + base + sub(suffix);
        if (number && prefix.find(L'#') == std::wstring::npos && suffix.find(L'#') == std::wstring::npos) name += L" " + num;
        st->newNames.push_back(name + ext);
    }
    // validate
    std::set<std::wstring> seen;
    for (size_t i = 0; i < st->newNames.size() && st->problem.empty(); ++i) {
        const std::wstring& n = st->newNames[i];
        if (n.empty() || n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) st->problem = L"\"" + n + L"\" isn't a valid name.";
        std::wstring key = ToLower(PathJoin(PathParent(st->paths[i]), n));
        if (!seen.insert(key).second) st->problem = L"Two items would both be named \"" + n + L"\". Turn on numbering or change the pattern.";
    }
    // fill the preview list
    HWND list = GetDlgItem(dlg, IdList);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    size_t changed = 0;
    for (size_t i = 0; i < st->paths.size() && i < 2000; ++i) {
        std::wstring oldName = PathLeaf(st->paths[i]);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)oldName.c_str();
        ListView_InsertItem(list, &it);
        ListView_SetItemText(list, (int)i, 1, (LPWSTR)st->newNames[i].c_str());
        if (st->newNames[i] != oldName) ++changed;
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    std::wstring status = st->problem.empty() ? std::to_wstring(changed) + L" of " + std::to_wstring(st->paths.size()) + L" items will be renamed." : st->problem;
    SetDlgItemTextW(dlg, IdStatus, status.c_str());
    EnableWindow(GetDlgItem(dlg, IDOK), st->problem.empty() && changed > 0);
}

static INT_PTR CALLBACK BatchProc(HWND dlg, UINT m, WPARAM w, LPARAM l) {
    auto* st = (BatchState*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (m) {
        case WM_INITDIALOG: {
            st = (BatchState*)l;
            SetWindowLongPtrW(dlg, DWLP_USER, l);
            SetWindowTextW(dlg, (L"Rename " + std::to_wstring(st->paths.size()) + L" items").c_str());
            HWND list = GetDlgItem(dlg, IdList);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            RECT rc;
            GetClientRect(list, &rc);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = (rc.right - 20) / 2;
            col.pszText = (LPWSTR)L"Current name";
            ListView_InsertColumn(list, 0, &col);
            col.pszText = (LPWSTR)L"New name";
            ListView_InsertColumn(list, 1, &col);
            HWND cb = GetDlgItem(dlg, IdCaseMode);
            for (const wchar_t* s : {L"Keep case", L"lowercase", L"UPPERCASE", L"Title Case"}) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)s);
            SendMessageW(cb, CB_SETCURSEL, 0, 0);
            SetDlgItemTextW(dlg, IdStart, L"1");
            CheckDlgButton(dlg, IdExt, BST_CHECKED);
            ThemeWin32Dialog(dlg);
            Recompute(dlg, st);
            SetFocus(GetDlgItem(dlg, IdFind));
            return FALSE;
        }
        case WM_COMMAND: {
            WORD id = LOWORD(w), code = HIWORD(w);
            if (id == IDOK) {
                Recompute(dlg, st);
                if (!st->problem.empty()) return TRUE;
                for (size_t i = 0; i < st->paths.size(); ++i)
                    if (st->newNames[i] != PathLeaf(st->paths[i])) st->result.push_back({st->paths[i], PathJoin(PathParent(st->paths[i]), st->newNames[i])});
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
            if ((code == EN_CHANGE || code == BN_CLICKED || code == CBN_SELCHANGE) && id >= IdFind && id <= IdExt) Recompute(dlg, st);
            break;
        }
    }
    return FALSE;
}

std::vector<std::pair<std::wstring, std::wstring>> BatchRenameDialog(HWND owner, const std::vector<std::wstring>& paths) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    std::vector<WORD> t;
    auto align = [&] { while ((t.size() * 2) % 4) t.push_back(0); };
    auto str = [&](const wchar_t* s) { while (*s) t.push_back(*s++); t.push_back(0); };
    auto dw = [&](DWORD d) { t.push_back(LOWORD(d)); t.push_back(HIWORD(d)); };
    const int W = 380, H = 300;
    dw(DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    dw(0);
    WORD countPos = (WORD)t.size();
    t.push_back(0);
    t.push_back(0); t.push_back(0); t.push_back(W); t.push_back(H);
    t.push_back(0); t.push_back(0);
    str(L"");
    t.push_back(9);
    str(L"Segoe UI");
    int count = 0;
    auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, const wchar_t* cls, const wchar_t* text) {
        align();
        dw(style | WS_CHILD | WS_VISIBLE);
        dw(0);
        t.push_back(x); t.push_back(y); t.push_back(cx); t.push_back(cy);
        t.push_back(id);
        if ((uintptr_t)cls <= 0xFFFF) { t.push_back(0xFFFF); t.push_back((WORD)(uintptr_t)cls); } else str(cls);
        str(text);
        t.push_back(0);
        ++count;
    };
    const wchar_t* BTN = (const wchar_t*)(uintptr_t)0x0080;
    const wchar_t* EDT = (const wchar_t*)(uintptr_t)0x0081;
    const wchar_t* STC = (const wchar_t*)(uintptr_t)0x0082;
    const wchar_t* CMB = (const wchar_t*)(uintptr_t)0x0085;
    item(SS_LEFT, 8, 10, 40, 10, 0xFFFF, STC, L"Find");
    item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 50, 8, 130, 13, IdFind, EDT, L"");
    item(SS_LEFT, 192, 10, 44, 10, 0xFFFF, STC, L"Replace");
    item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 238, 8, 134, 13, IdReplace, EDT, L"");
    item(BS_AUTOCHECKBOX | WS_TABSTOP, 50, 25, 110, 11, IdRegex, BTN, L"Regular expression");
    item(BS_AUTOCHECKBOX | WS_TABSTOP, 162, 25, 80, 11, IdCase, BTN, L"Match case");
    item(BS_AUTOCHECKBOX | WS_TABSTOP, 244, 25, 128, 11, IdExt, BTN, L"Keep file extensions");
    item(SS_LEFT, 8, 44, 40, 10, 0xFFFF, STC, L"Prefix");
    item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 50, 42, 130, 13, IdPrefix, EDT, L"");
    item(SS_LEFT, 192, 44, 44, 10, 0xFFFF, STC, L"Suffix");
    item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 238, 42, 134, 13, IdSuffix, EDT, L"");
    item(BS_AUTOCHECKBOX | WS_TABSTOP, 50, 61, 70, 11, IdNumber, BTN, L"Number from");
    item(ES_AUTOHSCROLL | ES_NUMBER | WS_BORDER | WS_TABSTOP, 122, 59, 30, 13, IdStart, EDT, L"");
    item(SS_LEFT, 158, 61, 90, 10, 0xFFFF, STC, L"(# in prefix/suffix)");
    item(CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 272, 59, 100, 80, IdCaseMode, CMB, L"");
    item(LVS_REPORT | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 8, 80, W - 16, H - 112, IdList, L"SysListView32", L"");
    item(SS_LEFT, 8, H - 25, 230, 20, IdStatus, STC, L"");
    item(BS_DEFPUSHBUTTON | WS_TABSTOP, W - 112, H - 26, 50, 16, IDOK, BTN, L"Rename");
    item(BS_PUSHBUTTON | WS_TABSTOP, W - 58, H - 26, 50, 16, IDCANCEL, BTN, L"Cancel");
    t[countPos] = (WORD)count;
    BatchState st;
    st.paths = paths;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)t.data(), owner, BatchProc, (LPARAM)&st);
    return st.result;
}

std::vector<std::pair<std::wstring, std::wstring>> BatchRenameApply(const std::vector<std::pair<std::wstring, std::wstring>>& plan,
                                                                     std::wstring* error) {
    std::vector<std::pair<std::wstring, std::wstring>> done;
    std::vector<std::pair<std::wstring, std::wstring>> staged;   // (temp, final)
    // pass 1: move everything to temporary names (so a<->b swaps can't collide)
    for (auto& p : plan) {
        std::wstring tmp = PathJoin(PathParent(p.first), L".strata-rename-" + std::to_wstring(GetTickCount()) + L"-" + std::to_wstring(staged.size()));
        if (!MoveFileExW(p.first.c_str(), tmp.c_str(), 0)) {
            if (error) *error = L"Couldn't rename \"" + PathLeaf(p.first) + L"\" (" + std::to_wstring(GetLastError()) + L")";
            break;
        }
        staged.push_back({tmp, p.second});
        done.push_back(p);
    }
    // pass 2: final names (an existing unrelated item with the target name gets " (2)")
    for (size_t i = 0; i < staged.size(); ++i) {
        std::wstring target = staged[i].second;
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring leaf = PathLeaf(target), base = leaf, ext;
            size_t dot = leaf.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) { base = leaf.substr(0, dot); ext = leaf.substr(dot); }
            for (int k = 2; GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && k < 1000; ++k)
                target = PathJoin(PathParent(target), base + L" (" + std::to_wstring(k) + L")" + ext);
            done[i].second = target;
        }
        if (!MoveFileExW(staged[i].first.c_str(), target.c_str(), 0)) {
            MoveFileExW(staged[i].first.c_str(), done[i].first.c_str(), 0);   // put it back
            done[i].second = done[i].first;
        }
    }
    // roll back the items pass 1 couldn't handle are untouched; drop no-op entries
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (auto& d : done) if (_wcsicmp(d.first.c_str(), d.second.c_str()) != 0) out.push_back(d);
    return out;
}
