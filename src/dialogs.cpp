// Strata - themed dialogs (see dialogs.h)
#include "dialogs.h"
#include "config.h"
#include "gfx.h"
#include "textinput.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

using namespace gfx;

namespace {
enum Hit { HitNone = -1, HitCheck = 100, HitInput = 200 };

struct DlgState {
    DlgSpec spec;
    DlgResult res;
    bool done = false;
    HWND hwnd = nullptr;
    UINT dpi = 96;
    float scale = 1;
    Theme th;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    Canvas cv;
    IDWriteTextFormat *fHead = nullptr, *fBody = nullptr, *fBtn = nullptr, *fIcon = nullptr, *fInput = nullptr, *fSmallIcon = nullptr;
    TextInput input;
    // layout (DIPs)
    float W = 440, H = 200;
    D2D1_RECT_F rcIcon{}, rcHead{}, rcBody{}, rcCheck{}, rcInput{}, rcFooter{};
    std::vector<D2D1_RECT_F> rcBtn;
    float bodyH = 0, bodyScroll = 0;
    int hot = HitNone, pressed = HitNone, focus = HitNone;
    bool focusVisible = false;   // keyboard focus ring (after Tab / arrows)
};

constexpr UINT_PTR kCaretTimer = 1;
const wchar_t* kClass = L"StrataDialog";

D2D1_RECT_F RF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
bool InR(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
}  // namespace

static void ReleaseFormats(DlgState& s) {
    SafeRelease(s.fHead); SafeRelease(s.fBody); SafeRelease(s.fBtn); SafeRelease(s.fIcon); SafeRelease(s.fInput); SafeRelease(s.fSmallIcon);
}

static void CreateFormats(DlgState& s) {
    std::wstring fam = g_config.fontFamily.empty() ? UiFont() : g_config.fontFamily;
    float k = std::max(0.85f, g_config.fontSize / 13.0f);
    s.fHead = Format(fam.c_str(), 15.5f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
    s.fBody = Format(fam.c_str(), 13 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
    s.fBtn = Format(fam.c_str(), 13 * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    s.fInput = Format(fam.c_str(), 13 * k, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    s.fIcon = Format(IconFont(), 18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    s.fSmallIcon = Format(IconFont(), 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    if (s.fIcon) s.fIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (s.fSmallIcon) s.fSmallIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (s.fBtn) s.fBtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (s.fInput) s.fInput->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

static float ButtonW(DlgState& s, const DlgButton& b) { return std::max(96.0f, MeasureWidth(b.label, s.fBtn) + 36); }

static void Layout(DlgState& s) {
    const float pad = 24;
    float btnTotal = 0;
    for (auto& b : s.spec.buttons) btnTotal += ButtonW(s, b) + 8;
    s.W = std::max(s.spec.width, btnTotal + pad * 2);
    bool icon = s.spec.icon != DlgSpec::None;
    float x0 = pad + (icon ? 48 : 0), x1 = s.W - pad, textW = x1 - x0;
    float y = pad;
    s.rcIcon = RF(pad, y - 2, pad + 36, y + 34);
    float headH = s.spec.heading.empty() ? 0 : MeasureHeight(s.spec.heading, s.fHead, textW);
    s.rcHead = RF(x0, y, x1, y + headH);
    y += headH;
    if (!s.spec.body.empty()) {
        y += headH > 0 ? 8 : 0;
        s.bodyH = MeasureHeight(s.spec.body, s.fBody, textW - 8) + 2;
        float viewH = std::min(s.bodyH, 340.0f);
        s.rcBody = RF(x0, y, x1, y + viewH);
        y += viewH;
    } else {
        s.rcBody = RF(x0, y, x1, y);
    }
    if (icon) y = std::max(y, s.rcIcon.bottom);
    y += 18;
    if (s.spec.input) {
        s.rcInput = RF(x0, y, x1, y + 34);
        y += 34 + 14;
    }
    if (!s.spec.checkbox.empty()) {
        s.rcCheck = RF(x0, y, std::min(x1, x0 + 34 + MeasureWidth(s.spec.checkbox, s.fInput)), y + 22);
        y += 22 + 14;
    }
    y += 4;
    s.rcFooter = RF(0, y, s.W, y + 68);
    s.rcBtn.clear();
    float bx = s.W - pad;
    std::vector<D2D1_RECT_F> rs;
    for (int i = (int)s.spec.buttons.size() - 1; i >= 0; --i) {
        float w = ButtonW(s, s.spec.buttons[i]);
        rs.insert(rs.begin(), RF(bx - w, y + 18, bx, y + 50));
        bx -= w + 8;
    }
    s.rcBtn = rs;
    s.H = y + 68;
}

static int DefaultButton(const DlgState& s) {
    for (size_t i = 0; i < s.spec.buttons.size(); ++i) if (s.spec.buttons[i].primary) return (int)i;
    return 0;
}

static void Finish(DlgState& s, int id) {
    s.res.id = id;
    s.res.checked = s.spec.checked;
    s.res.text = s.input.text;
    s.done = true;
}

static int HitTestDlg(DlgState& s, float x, float y) {
    for (size_t i = 0; i < s.rcBtn.size(); ++i) if (InR(s.rcBtn[i], x, y)) return (int)i;
    if (!s.spec.checkbox.empty() && InR(s.rcCheck, x, y)) return HitCheck;
    if (s.spec.input && InR(s.rcInput, x, y)) return HitInput;
    return HitNone;
}

static void Paint(DlgState& s) {
    if (!s.rt) {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(s.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &s.rt))) return;
        s.rt->SetDpi((float)s.dpi, (float)s.dpi);
        s.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        s.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &s.br);
        s.cv.rt = s.rt;
        s.cv.br = s.br;
    }
    const Theme& t = s.th;
    D2D1_COLOR_F bg = Mix(t.bg, t.surface, t.dark ? 0.35f : 0.0f);
    bg.a = 1;
    D2D1_COLOR_F foot = t.bottomBg;
    foot.a = 1;
    s.rt->BeginDraw();
    s.rt->Clear(bg);
    // icon
    if (s.spec.icon != DlgSpec::None) {
        struct { wchar_t g; uint32_t c; } ic[] = {{0, 0}, {0xE946, 0}, {0xE7BA, 0xF5A524}, {0xE9CE, 0}, {0xE7BA, 0xE5484D}, {0xE896, 0}};
        auto& i = ic[s.spec.icon];
        D2D1_COLOR_F c = i.c ? Rgb(i.c) : t.accent;
        float cx = (s.rcIcon.left + s.rcIcon.right) / 2, cy = (s.rcIcon.top + s.rcIcon.bottom) / 2;
        s.cv.Circle(cx, cy, 18, Alpha(c, 0.18f));
        s.cv.Glyph(i.g, s.fIcon, s.rcIcon, c);
    }
    s.cv.Text(s.spec.heading, s.fHead, RF(s.rcHead.left, s.rcHead.top, s.rcHead.right, s.rcHead.bottom + 4), t.text);
    if (!s.spec.body.empty()) {
        float viewH = s.rcBody.bottom - s.rcBody.top;
        float maxS = std::max(0.0f, s.bodyH - viewH);
        s.bodyScroll = std::clamp(s.bodyScroll, 0.0f, maxS);
        s.rt->PushAxisAlignedClip(s.rcBody, D2D1_ANTIALIAS_MODE_ALIASED);
        s.cv.Text(s.spec.body, s.fBody, RF(s.rcBody.left, s.rcBody.top - s.bodyScroll, s.rcBody.right - 8, s.rcBody.top - s.bodyScroll + s.bodyH + 4),
                  Alpha(t.text, 0.82f));
        s.rt->PopAxisAlignedClip();
        if (maxS > 0) {   // thin scroll indicator
            float th = std::max(24.0f, viewH * viewH / s.bodyH);
            float ty = s.rcBody.top + (viewH - th) * (s.bodyScroll / maxS);
            s.cv.FillRound(RF(s.rcBody.right - 3, ty, s.rcBody.right, ty + th), 1.5f, Alpha(t.text, 0.25f));
        }
    }
    if (s.spec.input) {
        bool f = s.focus == HitInput;
        s.cv.FillRound(s.rcInput, 6, f ? t.surfaceStrong : t.surface);
        s.cv.StrokeRound(s.rcInput, 6, f ? t.accent : t.divider, f ? 1.4f : 1.0f);
        D2D1_RECT_F box = RF(s.rcInput.left + 10, s.rcInput.top, s.rcInput.right - 10, s.rcInput.bottom);
        if (s.input.text.empty() && !s.spec.inputHint.empty()) s.cv.Text(s.spec.inputHint, s.fInput, box, Alpha(t.subtext, 0.7f));
        s.input.Draw(s.cv, s.fInput, box, t.text, Alpha(t.accent, 0.35f), t.text, f);
    }
    if (!s.spec.checkbox.empty()) {
        float cy = (s.rcCheck.top + s.rcCheck.bottom) / 2;
        D2D1_RECT_F box = RF(s.rcCheck.left, cy - 9, s.rcCheck.left + 18, cy + 9);
        if (s.spec.checked) {
            s.cv.FillRound(box, 4, t.accent);
            // check mark
            s.br->SetColor(t.onAccent);
            s.rt->DrawLine(D2D1::Point2F(box.left + 4.5f, cy + 0.5f), D2D1::Point2F(box.left + 7.5f, cy + 3.5f), s.br, 1.8f);
            s.rt->DrawLine(D2D1::Point2F(box.left + 7.5f, cy + 3.5f), D2D1::Point2F(box.left + 13.5f, cy - 3.5f), s.br, 1.8f);
        } else {
            s.cv.FillRound(box, 4, s.hot == HitCheck ? t.hover : Alpha(t.surface, 0.6f));
            s.cv.StrokeRound(box, 4, Alpha(t.text, 0.45f), 1.2f);
        }
        if (s.focusVisible && s.focus == HitCheck) s.cv.StrokeRound(RF(box.left - 3, box.top - 3, box.right + 3, box.bottom + 3), 6, t.accent, 1.5f);
        s.cv.Text(s.spec.checkbox, s.fInput, RF(box.right + 10, s.rcCheck.top, s.rcCheck.right + 40, s.rcCheck.bottom), t.text);
    }
    // footer
    s.cv.Fill(s.rcFooter, foot);
    s.cv.Fill(RF(0, s.rcFooter.top, s.W, s.rcFooter.top + 0.5f), t.divider);
    for (size_t i = 0; i < s.rcBtn.size(); ++i) {
        const DlgButton& b = s.spec.buttons[i];
        D2D1_RECT_F r = s.rcBtn[i];
        bool hot = s.hot == (int)i, down = s.pressed == (int)i && hot;
        D2D1_COLOR_F fill, text;
        if (b.danger) { fill = Rgb(0xC42B1C); text = Rgb(0xFFFFFF); }
        else if (b.primary) { fill = t.accent; text = t.onAccent; }
        else { fill = t.surfaceStrong; text = t.text; }
        if (hot) fill = Mix(fill, t.dark ? Rgb(0xFFFFFF) : Rgb(0x000000), down ? 0.16f : 0.08f);
        s.cv.FillRound(r, 6, fill);
        if (!b.primary && !b.danger) s.cv.StrokeRound(r, 6, t.divider, 1);
        if (s.focusVisible && s.focus == (int)i)
            s.cv.StrokeRound(RF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), 8, t.accent, 1.6f);
        s.cv.Text(b.label, s.fBtn, RF(r.left + 6, r.top, r.right - 6, r.bottom), text);
    }
    if (s.rt->EndDraw() == D2DERR_RECREATE_TARGET) { SafeRelease(s.br); SafeRelease(s.rt); InvalidateRect(s.hwnd, nullptr, FALSE); }
}

static void MoveFocus(DlgState& s, int dir) {
    std::vector<int> order;
    if (s.spec.input) order.push_back(HitInput);
    if (!s.spec.checkbox.empty()) order.push_back(HitCheck);
    for (size_t i = 0; i < s.spec.buttons.size(); ++i) order.push_back((int)i);
    if (order.empty()) return;
    auto it = std::find(order.begin(), order.end(), s.focus);
    int k = it == order.end() ? (dir > 0 ? -1 : 0) : (int)(it - order.begin());
    k = (k + dir + (int)order.size()) % (int)order.size();
    s.focus = order[k];
    s.focusVisible = true;
}

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<DlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    auto dip = [&](LPARAM l, float& x, float& y) { x = GET_X_LPARAM(l) / s->scale; y = GET_Y_LPARAM(l) / s->scale; };
    switch (msg) {
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            float x = pt.x / s->scale, y = pt.y / s->scale;
            if (HitTestDlg(*s, x, y) != HitNone) return HTCLIENT;
            if (!s->spec.body.empty() && InR(s->rcBody, x, y) && s->bodyH > s->rcBody.bottom - s->rcBody.top) return HTCLIENT;
            return HTCAPTION;   // drag the dialog by its background
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint(*s);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            float x, y;
            dip(lp, x, y);
            int h = HitTestDlg(*s, x, y);
            if (h != s->hot) { s->hot = h; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            SetCursor(LoadCursorW(nullptr, h == HitInput ? IDC_IBEAM : IDC_ARROW));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;
            break;
        case WM_MOUSELEAVE: s->hot = HitNone; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: {
            float x, y;
            dip(lp, x, y);
            s->pressed = HitTestDlg(*s, x, y);
            if (s->pressed == HitInput) {
                s->focus = HitInput;
                size_t p = s->input.HitTest(s->fInput, RF(s->rcInput.left + 10, s->rcInput.top, s->rcInput.right - 10, s->rcInput.bottom), x);
                s->input.Select(p, p);
            }
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            float x, y;
            dip(lp, x, y);
            int h = HitTestDlg(*s, x, y);
            int was = s->pressed;
            s->pressed = HitNone;
            ReleaseCapture();
            if (h == was) {
                if (h >= 0 && h < (int)s->spec.buttons.size()) Finish(*s, s->spec.buttons[h].id);
                else if (h == HitCheck) { s->spec.checked = !s->spec.checked; s->focus = HitCheck; }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            s->bodyScroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 48;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN: {
            UINT vk = (UINT)wp;
            if (vk == VK_ESCAPE) { Finish(*s, s->spec.cancelId); return 0; }
            if (vk == VK_RETURN) {
                int b = (s->focus >= 0 && s->focus < (int)s->spec.buttons.size()) ? s->focus : DefaultButton(*s);
                if (b < (int)s->spec.buttons.size()) Finish(*s, s->spec.buttons[b].id);
                return 0;
            }
            if (vk == VK_TAB) { MoveFocus(*s, GetKeyState(VK_SHIFT) < 0 ? -1 : 1); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (s->focus == HitInput) {
                bool handled = false;
                s->input.OnKey(hwnd, vk, handled);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if ((vk == VK_LEFT || vk == VK_RIGHT) && s->focus >= 0) { MoveFocus(*s, vk == VK_LEFT ? -1 : 1); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (vk == VK_UP || vk == VK_DOWN) { s->bodyScroll += vk == VK_UP ? -40.0f : 40.0f; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (vk == VK_SPACE) {
                if (s->focus == HitCheck) s->spec.checked = !s->spec.checked;
                else if (s->focus >= 0 && s->focus < (int)s->spec.buttons.size()) Finish(*s, s->spec.buttons[s->focus].id);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (vk == 'C' && GetKeyState(VK_CONTROL) < 0) {   // copy the message, like Windows message boxes
                std::wstring all = s->spec.heading + L"\r\n\r\n" + s->spec.body;
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    size_t bytes = (all.size() + 1) * sizeof(wchar_t);
                    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (g) { memcpy(GlobalLock(g), all.c_str(), bytes); GlobalUnlock(g); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); }
                    CloseClipboard();
                }
            }
            return 0;
        }
        case WM_CHAR:
            if (s->focus == HitInput && wp >= 0x20) { s->input.OnChar((wchar_t)wp); InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_TIMER:
            if (wp == kCaretTimer && s->focus == HitInput) { s->input.caretOn = !s->input.caretOn; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_ACTIVATE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_CLOSE:
            Finish(*s, s->spec.cancelId);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DlgResult ShowDialog(HWND owner, const DlgSpec& specIn) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    DlgState s;
    s.spec = specIn;
    if (s.spec.buttons.empty()) s.spec.buttons.push_back({IDOK, L"OK", true});
    if (!s.spec.cancelId) s.spec.cancelId = s.spec.buttons.size() == 1 ? s.spec.buttons[0].id : s.spec.buttons.back().id;
    s.th = BuildTheme(g_config);
    HWND root = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    s.dpi = root ? GetDpiForWindow(root) : GetDpiForSystem();
    s.scale = s.dpi / 96.0f;
    CreateFormats(s);
    s.input.SetText(s.spec.inputText);
    s.input.SelectAll();
    s.focus = s.spec.input ? HitInput : HitNone;
    Layout(s);

    int w = (int)std::ceil(s.W * s.scale), h = (int)std::ceil(s.H * s.scale);
    RECT anchor{};
    if (root && IsWindowVisible(root) && !IsIconic(root)) GetWindowRect(root, &anchor);
    HMONITOR mon = root ? MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (anchor.right <= anchor.left) anchor = mi.rcWork;
    int x = anchor.left + ((anchor.right - anchor.left) - w) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - h) / 2 - (int)(30 * s.scale);
    x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
    y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - h));

    s.hwnd = CreateWindowExW(root ? 0 : WS_EX_APPWINDOW, kClass, L"Strata", WS_POPUP, x, y, w, h, root, nullptr, GetModuleHandleW(nullptr), &s);
    if (!s.hwnd) { ReleaseFormats(s); return {s.spec.cancelId, s.spec.checked, s.spec.inputText}; }
    ApplyWindowTheme(s.hwnd, s.th, true);
    if (s.spec.icon == DlgSpec::Warning || s.spec.icon == DlgSpec::Danger) MessageBeep(MB_ICONWARNING);
    bool ownerWasEnabled = root && IsWindowEnabled(root);
    if (ownerWasEnabled) EnableWindow(root, FALSE);
    ShowWindow(s.hwnd, SW_SHOW);
    SetForegroundWindow(s.hwnd);
    SetFocus(s.hwnd);
    SetTimer(s.hwnd, kCaretTimer, GetCaretBlinkTime(), nullptr);

    MSG msg;
    bool quit = false;
    WPARAM quitCode = 0;
    while (!s.done) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) { quit = true; quitCode = msg.wParam; Finish(s, s.spec.cancelId); break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // re-enable the owner before the dialog goes so Windows hands focus back to it (not another app)
    if (ownerWasEnabled) EnableWindow(root, TRUE);
    KillTimer(s.hwnd, kCaretTimer);
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(s.hwnd);
    if (root) SetForegroundWindow(root);
    SafeRelease(s.br);
    SafeRelease(s.rt);
    ReleaseFormats(s);
    if (quit) PostQuitMessage((int)quitCode);
    return s.res;
}

bool ConfirmDialog(HWND owner, const std::wstring& heading, const std::wstring& body, const std::wstring& okLabel, bool danger, DlgSpec::Icon icon) {
    DlgSpec d;
    d.heading = heading;
    d.body = body;
    d.icon = danger ? DlgSpec::Danger : icon;
    d.buttons = {{IDOK, okLabel, !danger, danger}, {IDCANCEL, L"Cancel"}};
    d.cancelId = IDCANCEL;
    return ShowDialog(owner, d).id == IDOK;
}

void NoticeDialog(HWND owner, const std::wstring& heading, const std::wstring& body, bool warning) {
    DlgSpec d;
    d.heading = heading;
    d.body = body;
    d.icon = warning ? DlgSpec::Warning : DlgSpec::Info;
    d.buttons = {{IDOK, L"OK", true}};
    ShowDialog(owner, d);
}

bool TextDialog(HWND owner, const std::wstring& heading, const std::wstring& body, std::wstring& value, const std::wstring& okLabel) {
    DlgSpec d;
    d.heading = heading;
    d.body = body;
    d.input = true;
    d.inputText = value;
    d.buttons = {{IDOK, okLabel, true}, {IDCANCEL, L"Cancel"}};
    d.cancelId = IDCANCEL;
    DlgResult r = ShowDialog(owner, d);
    if (r.id != IDOK) return false;
    value = r.text;
    return true;
}

// ============================================================ classic dialogs in the theme's colors
namespace {
struct DlgColors {
    HBRUSH bg = nullptr, edit = nullptr;
    COLORREF bgc = 0, editc = 0, text = 0;
};
}  // namespace

static COLORREF Ref(D2D1_COLOR_F c, D2D1_COLOR_F under) {
    // flatten a translucent theme color onto the opaque one beneath it
    float a = c.a;
    return RGB((int)((under.r * (1 - a) + c.r * a) * 255), (int)((under.g * (1 - a) + c.g * a) * 255), (int)((under.b * (1 - a) + c.b * a) * 255));
}

static void AllowDark(HWND h, bool dark) {
    static auto fn = reinterpret_cast<bool(WINAPI*)(HWND, bool)>(
        GetProcAddress(GetModuleHandleW(L"uxtheme.dll") ? GetModuleHandleW(L"uxtheme.dll") : LoadLibraryW(L"uxtheme.dll"), MAKEINTRESOURCEA(133)));
    if (fn) fn(h, dark);
}

static LRESULT CALLBACK ThemedDlgSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    auto* c = reinterpret_cast<DlgColors*>(ref);
    switch (m) {
        case WM_CTLCOLORDLG: return (LRESULT)c->bg;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)w, c->text);
            SetBkColor((HDC)w, c->bgc);
            return (LRESULT)c->bg;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)w, c->text);
            SetBkColor((HDC)w, c->editc);
            return (LRESULT)c->edit;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, ThemedDlgSubclass, id);
            DeleteObject(c->bg);
            DeleteObject(c->edit);
            delete c;
            break;
    }
    return DefSubclassProc(h, m, w, l);
}

void ThemeWin32Dialog(HWND dlg) {
    Theme t = BuildTheme(g_config);
    D2D1_COLOR_F base = t.bg;
    base.a = 1;
    auto* c = new DlgColors;
    c->bgc = Ref(Mix(t.bg, t.surface, t.dark ? 0.35f : 0.0f), base);
    c->editc = Ref(t.surface, base);
    c->text = Ref(t.text, base);
    c->bg = CreateSolidBrush(c->bgc);
    c->edit = CreateSolidBrush(c->editc);
    SetWindowSubclass(dlg, ThemedDlgSubclass, 1, (DWORD_PTR)c);
    BOOL dark = t.dark;
    DwmSetWindowAttribute(dlg, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF cap = c->bgc;
    DwmSetWindowAttribute(dlg, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
    int corner = 2;
    DwmSetWindowAttribute(dlg, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    AllowDark(dlg, t.dark);
    struct Ctx { bool dark; DlgColors* c; } ctx{t.dark != FALSE, c};
    EnumChildWindows(dlg, [](HWND h, LPARAM lp) -> BOOL {
        auto* x = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        AllowDark(h, x->dark);
        if (!_wcsicmp(cls, L"Button")) {
            LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
            bool box = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON ||
                       type == BS_3STATE || type == BS_AUTO3STATE;
            // themed check boxes ignore the text color; the classic look takes the theme's colors
            if (box && x->dark) SetWindowTheme(h, L"", L"");
            else SetWindowTheme(h, x->dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        } else if (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"ComboBox")) {
            SetWindowTheme(h, x->dark ? L"DarkMode_CFD" : L"CFD", nullptr);
        } else if (!_wcsicmp(cls, WC_LISTVIEWW)) {
            SetWindowTheme(h, x->dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            ListView_SetBkColor(h, x->c->editc);
            ListView_SetTextBkColor(h, x->c->editc);
            ListView_SetTextColor(h, x->c->text);
            if (HWND hdr = ListView_GetHeader(h)) { AllowDark(hdr, x->dark); SetWindowTheme(hdr, x->dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr); }
        }
        SendMessageW(h, WM_THEMECHANGED, 0, 0);
        return TRUE;
    }, (LPARAM)&ctx);
    InvalidateRect(dlg, nullptr, TRUE);
}
