// Strata - themed popup menus (see menu.h)
#include "menu.h"
#include "config.h"
#include "gfx.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

using namespace gfx;

namespace {
struct MItem {
    UINT id = 0;
    std::wstring text, accel;
    wchar_t mnemonic = 0;
    bool sep = false, disabled = false, checked = false, radio = false, isDefault = false, caption = false;
    HMENU sub = nullptr;
    wchar_t glyph = 0;
    float y = 0, h = 0;
    bool Selectable() const { return !sep && !caption && !disabled; }
};

struct MWin {
    HWND hwnd = nullptr;
    HMENU menu = nullptr;
    std::vector<MItem> items;
    int hot = -1;
    int parentItem = -1;        // item in the parent menu that opened this one
    float w = 0, h = 0, leftCol = 36;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    Canvas cv;
};

struct MState {
    std::vector<MWin*> stack;   // root first, then open submenus
    HWND owner = nullptr;
    Theme th;
    UINT dpi = 96;
    float scale = 1;
    bool backdrop = false;
    IDWriteTextFormat *fText = nullptr, *fBold = nullptr, *fAccel = nullptr, *fIcon = nullptr, *fCaption = nullptr;
    UINT result = 0;
    bool done = false;
    DWORD openedAt = 0;
    POINT openPt{};
};

MState* g_ms = nullptr;
const wchar_t* kMenuClass = L"StrataMenu";
constexpr UINT_PTR kSubTimer = 1;
constexpr float kItemH = 32, kSepH = 9, kPad = 5;

D2D1_RECT_F RF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
}  // namespace

void MenuIcon(HMENU menu, UINT id, wchar_t glyph) {
    MENUITEMINFOW mi{sizeof(mi)};
    mi.fMask = MIIM_DATA;
    mi.dwItemData = glyph;
    SetMenuItemInfoW(menu, id, FALSE, &mi);
}

static void ReadItems(MWin& w) {
    int n = GetMenuItemCount(w.menu);
    std::vector<MItem> items;
    for (int i = 0; i < n; ++i) {
        wchar_t buf[512] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_DATA;
        mi.dwTypeData = buf;
        mi.cch = 511;
        if (!GetMenuItemInfoW(w.menu, i, TRUE, &mi)) continue;
        MItem it;
        it.id = mi.wID;
        it.sep = (mi.fType & MFT_SEPARATOR) != 0;
        it.disabled = (mi.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
        it.checked = (mi.fState & MFS_CHECKED) != 0;
        it.radio = (mi.fType & MFT_RADIOCHECK) != 0;
        it.isDefault = (mi.fState & MFS_DEFAULT) != 0;
        it.sub = mi.hSubMenu;
        if (mi.dwItemData >= 0xE000 && mi.dwItemData <= 0xF8FF) it.glyph = (wchar_t)mi.dwItemData;
        if (!it.sep) {
            std::wstring raw = buf;
            size_t tab = raw.find(L'\t');
            if (tab != std::wstring::npos) { it.accel = raw.substr(tab + 1); raw.resize(tab); }
            for (size_t k = 0; k < raw.size(); ++k) {
                if (raw[k] == L'&') {
                    if (k + 1 < raw.size() && raw[k + 1] == L'&') { it.text += L'&'; ++k; }
                    else if (k + 1 < raw.size() && !it.mnemonic) it.mnemonic = (wchar_t)towlower(raw[k + 1]);
                } else {
                    it.text += raw[k];
                }
            }
            // a greyed item with no command is a section caption ("Theme", "Files changed in …")
            it.caption = it.disabled && it.id == 0 && !it.sub;
        }
        // no doubled, leading or trailing separators
        if (it.sep && (items.empty() || items.back().sep)) continue;
        items.push_back(it);
    }
    while (!items.empty() && items.back().sep) items.pop_back();
    w.items = std::move(items);
}

static void Layout(MWin& w) {
    MState& s = *g_ms;
    bool icons = false, checks = false, subs = false;
    float textW = 0, accelW = 0;
    for (auto& it : w.items) {
        if (it.sep) continue;
        icons |= it.glyph != 0;
        checks |= it.checked;
        subs |= it.sub != nullptr;
        textW = std::max(textW, MeasureWidth(it.text, it.isDefault ? s.fBold : it.caption ? s.fCaption : s.fText));
        if (!it.accel.empty()) accelW = std::max(accelW, MeasureWidth(it.accel, s.fAccel));
    }
    // keep an icon/check column whenever any item could use it, so text lines up
    for (int i = 0, n = GetMenuItemCount(w.menu); i < n; ++i) {
        UINT st = GetMenuState(w.menu, i, MF_BYPOSITION);
        if (st != (UINT)-1 && (st & MF_CHECKED)) checks = true;
    }
    w.leftCol = (icons || checks) ? 38.0f : 16.0f;
    w.w = w.leftCol + textW + (accelW > 0 ? 36 + accelW : 0) + (subs ? 30 : 0) + 18;
    w.w = std::clamp(w.w, 200.0f, 560.0f);
    float y = kPad;
    for (auto& it : w.items) {
        it.y = y;
        it.h = it.sep ? kSepH : it.caption ? 28.0f : kItemH;
        y += it.h;
    }
    w.h = y + kPad;
}

static D2D1_COLOR_F MenuBg() {
    const Theme& t = g_ms->th;
    D2D1_COLOR_F c = t.dark ? Mix(t.bg, t.surface, 0.75f) : Mix(t.bg, Rgb(0xFFFFFF), 0.6f);
    c.a = g_ms->backdrop ? (t.dark ? 0.80f : 0.84f) : 1.0f;
    return c;
}

static void DrawCheck(Canvas& cv, float cx, float cy, D2D1_COLOR_F c, bool radio) {
    if (radio) { cv.Circle(cx, cy, 3.5f, c); return; }
    cv.br->SetColor(c);
    cv.rt->DrawLine(D2D1::Point2F(cx - 5, cy + 0.5f), D2D1::Point2F(cx - 1.5f, cy + 4), cv.br, 1.7f);
    cv.rt->DrawLine(D2D1::Point2F(cx - 1.5f, cy + 4), D2D1::Point2F(cx + 5.5f, cy - 4), cv.br, 1.7f);
}

static void DrawChevron(Canvas& cv, float cx, float cy, D2D1_COLOR_F c) {
    cv.br->SetColor(c);
    cv.rt->DrawLine(D2D1::Point2F(cx - 2, cy - 4.5f), D2D1::Point2F(cx + 2.5f, cy), cv.br, 1.4f);
    cv.rt->DrawLine(D2D1::Point2F(cx + 2.5f, cy), D2D1::Point2F(cx - 2, cy + 4.5f), cv.br, 1.4f);
}

static void PaintMenu(MWin& w) {
    MState& s = *g_ms;
    if (!w.rt) {
        RECT rc;
        GetClientRect(w.hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(w.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &w.rt))) return;
        w.rt->SetDpi((float)s.dpi, (float)s.dpi);
        w.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        w.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &w.br);
        w.cv.rt = w.rt;
        w.cv.br = w.br;
    }
    const Theme& t = s.th;
    w.rt->BeginDraw();
    w.rt->Clear(MenuBg());
    if (!s.backdrop || !SystemBackdropSupported()) w.cv.StrokeRound(RF(0.5f, 0.5f, w.w - 0.5f, w.h - 0.5f), 8, t.divider, 1);
    for (int i = 0; i < (int)w.items.size(); ++i) {
        const MItem& it = w.items[i];
        if (it.sep) {
            float cy = std::floor(it.y + it.h / 2) + 0.5f;
            w.cv.Fill(RF(10, cy - 0.5f, w.w - 10, cy + 0.5f), t.divider);
            continue;
        }
        if (it.caption) {
            w.cv.Text(it.text, s.fCaption, RF(w.leftCol, it.y, w.w - 12, it.y + it.h), Alpha(t.subtext, 0.85f));
            continue;
        }
        D2D1_RECT_F row = RF(5, it.y + 1, w.w - 5, it.y + it.h - 1);
        bool hot = i == w.hot && !it.disabled;
        bool open = false;
        for (size_t k = 1; k < s.stack.size(); ++k)
            if (s.stack[k - 1] == &w && s.stack[k]->parentItem == i) open = true;
        if (hot || open) {
            w.cv.FillRound(row, 5, t.dark ? Alpha(Rgb(0xFFFFFF), 0.075f) : Alpha(Rgb(0x000000), 0.055f));
            // a short accent pill on the left, like Windows 11 lists
            if (hot) w.cv.FillRound(RF(row.left, row.top + 9, row.left + 3, row.bottom - 9), 1.5f, t.accent);
        }
        D2D1_COLOR_F ink = it.disabled ? Alpha(t.subtext, 0.45f) : t.text;
        float cy = (row.top + row.bottom) / 2;
        float iconX = 8 + (w.leftCol - 8) / 2;
        if (it.checked) DrawCheck(w.cv, iconX + 1, cy, it.disabled ? ink : t.accent, it.radio);
        else if (it.glyph) w.cv.Glyph(it.glyph, s.fIcon, RF(iconX - 10, row.top, iconX + 10, row.bottom), it.disabled ? ink : hot ? t.text : Alpha(t.text, 0.75f));
        float right = w.w - 14 - (it.sub ? 18 : 0);
        if (!it.accel.empty()) {
            float aw = MeasureWidth(it.accel, s.fAccel);
            w.cv.Text(it.accel, s.fAccel, RF(right - aw, row.top, right, row.bottom), it.disabled ? Alpha(t.subtext, 0.35f) : Alpha(t.subtext, 0.8f));
            right -= aw + 20;
        }
        w.cv.Text(it.text, it.isDefault ? s.fBold : s.fText, RF(w.leftCol, row.top, right, row.bottom), ink);
        if (it.sub) DrawChevron(w.cv, w.w - 18, cy, it.disabled ? ink : Alpha(t.text, 0.7f));
    }
    if (w.rt->EndDraw() == D2DERR_RECREATE_TARGET) { SafeRelease(w.br); SafeRelease(w.rt); InvalidateRect(w.hwnd, nullptr, FALSE); }
}

static int Level(HWND hwnd) {
    if (!g_ms) return -1;
    for (int i = 0; i < (int)g_ms->stack.size(); ++i) if (g_ms->stack[i]->hwnd == hwnd) return i;
    return -1;
}
static bool IsOurs(HWND h) {
    if (!g_ms || !h) return false;
    for (auto* w : g_ms->stack) if (w->hwnd == h) return true;
    return false;
}

static int ItemAtY(const MWin& w, float y) {
    for (int i = 0; i < (int)w.items.size(); ++i)
        if (y >= w.items[i].y && y < w.items[i].y + w.items[i].h) return w.items[i].Selectable() || w.items[i].sub ? i : -1;
    return -1;
}

static void Repaint(MWin* w) { if (w && w->hwnd) InvalidateRect(w->hwnd, nullptr, FALSE); }

static void CloseFrom(int level) {
    MState& s = *g_ms;
    while ((int)s.stack.size() > std::max(1, level)) {
        MWin* w = s.stack.back();
        s.stack.pop_back();
        SafeRelease(w->br);
        SafeRelease(w->rt);
        DestroyWindow(w->hwnd);
        delete w;
    }
    for (auto* w : s.stack) Repaint(w);
}

static MWin* CreateMenuWin(HMENU menu, int x, int y, bool alignRight, bool alignBottom, bool center, const RECT* avoid, const RECT* parentRc) {
    MState& s = *g_ms;
    auto* w = new MWin;
    w->menu = menu;
    ReadItems(*w);
    if (w->items.empty()) { delete w; return nullptr; }
    Layout(*w);
    int pw = (int)std::ceil(w->w * s.scale), ph = (int)std::ceil(w->h * s.scale);
    POINT anchor{x, y};
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int px = x, py = y;
    if (parentRc) {
        // submenu: to the right of the parent, or to the left if there's no room
        px = parentRc->right - (int)(4 * s.scale);
        if (px + pw > wa.right) px = parentRc->left - pw + (int)(4 * s.scale);
        if (py + ph > wa.bottom) py = std::max((int)wa.top, (int)wa.bottom - ph);
    } else {
        if (alignRight) px -= pw;
        else if (center) px -= pw / 2;
        if (alignBottom) py -= ph;
        if (px + pw > wa.right) px = wa.right - pw;
        if (px < wa.left) px = wa.left;
        if (py + ph > wa.bottom) {
            // flip above the point (or above the button it dropped from)
            int above = (avoid ? avoid->top : y) - ph;
            py = above >= wa.top ? above : std::max((int)wa.top, (int)wa.bottom - ph);
        }
        if (py < wa.top) py = wa.top;
    }
    w->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kMenuClass, L"", WS_POPUP, px, py, pw, ph, s.owner, nullptr,
                              GetModuleHandleW(nullptr), w);
    if (!w->hwnd) { delete w; return nullptr; }
    // rounded corners + border; acrylic behind on Windows 11
    ApplyWindowTheme(w->hwnd, s.th, true);
    if (s.backdrop) {
        int type = 3;   // DWMSBT_TRANSIENTWINDOW (acrylic)
        DwmSetWindowAttribute(w->hwnd, 38, &type, sizeof(type));
        MARGINS m{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(w->hwnd, &m);
    }
    ShowWindow(w->hwnd, SW_SHOWNOACTIVATE);
    s.stack.push_back(w);
    return w;
}

static int FirstSelectable(const MWin& w, int from, int dir) {
    int n = (int)w.items.size();
    for (int k = 0; k < n; ++k) {
        int i = ((from + dir * k) % n + n) % n;
        if (w.items[i].Selectable()) return i;
    }
    return -1;
}

static void OpenSub(int level, int item, bool keyboard) {
    MState& s = *g_ms;
    if (level < 0 || level >= (int)s.stack.size()) return;
    MWin* parent = s.stack[level];
    if (item < 0 || item >= (int)parent->items.size() || !parent->items[item].sub || parent->items[item].disabled) return;
    if ((int)s.stack.size() > level + 1 && s.stack[level + 1]->parentItem == item) {
        if (keyboard) { MWin* c = s.stack[level + 1]; c->hot = FirstSelectable(*c, 0, 1); Repaint(c); }
        return;
    }
    CloseFrom(level + 1);
    RECT prc;
    GetWindowRect(parent->hwnd, &prc);
    int y = prc.top + (int)((parent->items[item].y - kPad) * s.scale);
    MWin* c = CreateMenuWin(parent->items[item].sub, prc.right, y, false, false, false, nullptr, &prc);
    if (!c) return;
    c->parentItem = item;
    if (keyboard) c->hot = FirstSelectable(*c, 0, 1);
    Repaint(parent);
    Repaint(c);
}

static void Choose(UINT id) {
    g_ms->result = id;
    g_ms->done = true;
}

static void Activate(int level, int item, bool keyboard) {
    MState& s = *g_ms;
    if (level < 0 || level >= (int)s.stack.size()) return;
    MWin* w = s.stack[level];
    if (item < 0 || item >= (int)w->items.size()) return;
    const MItem& it = w->items[item];
    if (it.disabled || it.sep || it.caption) return;
    if (it.sub) { OpenSub(level, item, keyboard); return; }
    Choose(it.id);
}

static LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = reinterpret_cast<MWin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!w || !g_ms) return DefWindowProcW(hwnd, msg, wp, lp);
    MState& s = *g_ms;
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            PaintMenu(*w);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            float y = GET_Y_LPARAM(lp) / s.scale;
            int i = ItemAtY(*w, y);
            if (i >= 0 && w->items[i].disabled) i = -1;
            int level = Level(hwnd);
            if (i != w->hot) {
                w->hot = i;
                Repaint(w);
                // the menus above keep their opening item highlighted
                for (int k = 0; k < level; ++k) { s.stack[k]->hot = s.stack[k + 1]->parentItem; Repaint(s.stack[k]); }
                SetTimer(hwnd, kSubTimer, 230, nullptr);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE: {
            int level = Level(hwnd);
            bool childOpen = level + 1 < (int)s.stack.size();
            if (!childOpen) { w->hot = -1; Repaint(w); }
            return 0;
        }
        case WM_TIMER:
            if (wp == kSubTimer) {
                KillTimer(hwnd, kSubTimer);
                int level = Level(hwnd);
                if (level < 0) return 0;
                if (w->hot >= 0 && w->items[w->hot].sub) OpenSub(level, w->hot, false);
                else if (w->hot >= 0) CloseFrom(level + 1);   // moved to a plain item: close the submenu
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            int level = Level(hwnd);
            int i = ItemAtY(*w, GET_Y_LPARAM(lp) / s.scale);
            if (i >= 0 && w->items[i].sub) OpenSub(level, i, false);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            // ignore the release of the click that opened the menu
            POINT pt;
            GetCursorPos(&pt);
            if (GetTickCount() - s.openedAt < 250 && std::abs(pt.x - s.openPt.x) < 4 && std::abs(pt.y - s.openPt.y) < 4) return 0;
            int level = Level(hwnd);
            int i = ItemAtY(*w, GET_Y_LPARAM(lp) / s.scale);
            if (i >= 0) Activate(level, i, false);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void HandleKey(WPARAM vk) {
    MState& s = *g_ms;
    int level = (int)s.stack.size() - 1;
    MWin* w = s.stack[level];
    // keyboard goes to the deepest menu that has a highlighted item (or the deepest one)
    switch (vk) {
        case VK_ESCAPE:
            if (level > 0) { CloseFrom(level); s.stack[level - 1]->hot = w->parentItem; Repaint(s.stack[level - 1]); }
            else { s.result = 0; s.done = true; }
            return;
        case VK_LEFT:
            if (level > 0) { CloseFrom(level); Repaint(s.stack[level - 1]); }
            return;
        case VK_RIGHT:
            if (w->hot >= 0 && w->items[w->hot].sub) OpenSub(level, w->hot, true);
            return;
        case VK_UP:
            w->hot = FirstSelectable(*w, w->hot < 0 ? (int)w->items.size() - 1 : w->hot - 1, -1);
            Repaint(w);
            return;
        case VK_DOWN:
            w->hot = FirstSelectable(*w, w->hot < 0 ? 0 : w->hot + 1, 1);
            Repaint(w);
            return;
        case VK_HOME: w->hot = FirstSelectable(*w, 0, 1); Repaint(w); return;
        case VK_END: w->hot = FirstSelectable(*w, (int)w->items.size() - 1, -1); Repaint(w); return;
        case VK_RETURN:
        case VK_SPACE:
            if (w->hot >= 0) Activate(level, w->hot, true);
            return;
        case VK_MENU:
        case VK_F10:
            s.result = 0;
            s.done = true;
            return;
    }
}

static void HandleChar(WPARAM ch) {
    MState& s = *g_ms;
    int level = (int)s.stack.size() - 1;
    MWin* w = s.stack[level];
    wchar_t c = (wchar_t)towlower((wchar_t)ch);
    for (int i = 0; i < (int)w->items.size(); ++i) {
        const MItem& it = w->items[i];
        wchar_t m = it.mnemonic ? it.mnemonic : (it.text.empty() ? 0 : (wchar_t)towlower(it.text[0]));
        if (it.Selectable() && m == c) { w->hot = i; Repaint(w); Activate(level, i, true); return; }
    }
}

UINT ThemedMenu(HMENU menu, UINT flags, int x, int y, HWND owner, const RECT* avoid) {
    if (!menu || g_ms) return 0;   // (no menus inside menus)
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kMenuClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    MState s;
    g_ms = &s;
    s.owner = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    s.th = BuildTheme(g_config);
    s.dpi = s.owner ? GetDpiForWindow(s.owner) : GetDpiForSystem();
    s.scale = s.dpi / 96.0f;
    s.backdrop = SystemBackdropSupported() && g_config.backdrop != L"solid";
    std::wstring fam = g_config.fontFamily.empty() ? UiFont() : g_config.fontFamily;
    float k = std::max(0.85f, g_config.fontSize / 13.0f);
    s.fText = Format(fam.c_str(), 13.5f * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    s.fBold = Format(fam.c_str(), 13.5f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    s.fAccel = Format(fam.c_str(), 12 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    s.fCaption = Format(fam.c_str(), 12 * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    s.fIcon = Format(IconFont(), 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    for (auto* f : {s.fText, s.fBold, s.fAccel, s.fCaption, s.fIcon})
        if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s.openedAt = GetTickCount();
    GetCursorPos(&s.openPt);

    MWin* root = CreateMenuWin(menu, x, y, (flags & TPM_RIGHTALIGN) != 0, (flags & TPM_BOTTOMALIGN) != 0, (flags & TPM_CENTERALIGN) != 0, avoid, nullptr);
    if (root) {
        while (!s.done) {
            MSG msg;
            if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                // another app came to the front: close, like a normal menu
                HWND fg = GetForegroundWindow();
                if (fg != s.owner && !IsOurs(fg)) { s.result = 0; break; }
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
                continue;
            }
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); s.result = 0; break; }
            bool ours = IsOurs(msg.hwnd);
            switch (msg.message) {
                case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
                case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
                case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN:
                    if (!ours) { s.result = 0; s.done = true; continue; }   // a click elsewhere closes the menu (and is used up)
                    break;
                case WM_MOUSEMOVE: case WM_NCMOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                case WM_LBUTTONUP: case WM_RBUTTONUP:
                    if (!ours) continue;   // the window underneath doesn't react while the menu is open
                    break;
                case WM_KEYDOWN: case WM_SYSKEYDOWN: HandleKey(msg.wParam); continue;
                case WM_CHAR: case WM_SYSCHAR: HandleChar(msg.wParam); continue;
                case WM_KEYUP: case WM_SYSKEYUP: continue;
                case WM_CANCELMODE: s.result = 0; s.done = true; continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    UINT result = s.result;
    while (!s.stack.empty()) {
        MWin* w = s.stack.back();
        s.stack.pop_back();
        SafeRelease(w->br);
        SafeRelease(w->rt);
        DestroyWindow(w->hwnd);
        delete w;
    }
    for (auto* f : {s.fText, s.fBold, s.fAccel, s.fCaption, s.fIcon}) if (f) f->Release();
    g_ms = nullptr;
    // the window underneath should notice where the mouse is now
    if (s.owner) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(s.owner, &pt);
        PostMessageW(s.owner, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
    }
    return result;
}
