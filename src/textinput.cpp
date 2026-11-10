// Strata - small self-drawn single-line text field
#include "textinput.h"
#include <algorithm>

using namespace gfx;

void TextInput::Insert(const std::wstring& s) {
    DeleteSel();
    std::wstring clean;
    for (wchar_t c : s) if (c >= 0x20 && c != 0x7F) clean += c;
    if (text.size() + clean.size() > 1024) clean.resize(1024 - std::min<size_t>(1024, text.size()));
    text.insert(caret, clean);
    caret += clean.size();
    anchor = caret;
}

bool TextInput::DeleteSel() {
    if (!HasSel()) return false;
    size_t a = SelStart(), b = SelEnd();
    text.erase(a, b - a);
    caret = anchor = a;
    return true;
}

static bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_'; }

size_t TextInput::WordLeft(size_t i) const {
    while (i > 0 && !IsWordChar(text[i - 1])) --i;
    while (i > 0 && IsWordChar(text[i - 1])) --i;
    return i;
}

size_t TextInput::WordRight(size_t i) const {
    while (i < text.size() && IsWordChar(text[i])) ++i;
    while (i < text.size() && !IsWordChar(text[i])) ++i;
    return i;
}

bool TextInput::OnKey(HWND hwnd, UINT vk, bool& handled) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    handled = true;
    caretOn = true;
    auto move = [&](size_t to) { caret = to; if (!shift) anchor = caret; };
    switch (vk) {
        case VK_LEFT:
            if (!shift && HasSel()) { caret = anchor = SelStart(); return false; }
            move(ctrl ? WordLeft(caret) : (caret ? caret - 1 : 0)); return false;
        case VK_RIGHT:
            if (!shift && HasSel()) { caret = anchor = SelEnd(); return false; }
            move(ctrl ? WordRight(caret) : std::min(text.size(), caret + 1)); return false;
        case VK_HOME: move(0); return false;
        case VK_END: move(text.size()); return false;
        case VK_BACK:
            if (DeleteSel()) return true;
            if (!caret) return false;
            {
                size_t from = ctrl ? WordLeft(caret) : caret - 1;
                text.erase(from, caret - from);
                caret = anchor = from;
            }
            return true;
        case VK_DELETE:
            if (DeleteSel()) return true;
            if (caret >= text.size()) return false;
            {
                size_t to = ctrl ? WordRight(caret) : caret + 1;
                text.erase(caret, to - caret);
                anchor = caret;
            }
            return true;
        default: break;
    }
    if (ctrl) {
        if (vk == 'A') { SelectAll(); return false; }
        if ((vk == 'C' || vk == 'X') && HasSel()) {
            std::wstring s = text.substr(SelStart(), SelEnd() - SelStart());
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                size_t bytes = (s.size() + 1) * sizeof(wchar_t);
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (g) { memcpy(GlobalLock(g), s.c_str(), bytes); GlobalUnlock(g); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); }
                CloseClipboard();
            }
            if (vk == 'X') { DeleteSel(); return true; }
            return false;
        }
        if (vk == 'V') {
            std::wstring s;
            if (OpenClipboard(hwnd)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) { const wchar_t* p = (const wchar_t*)GlobalLock(h); if (p) s = p; GlobalUnlock(h); }
                CloseClipboard();
            }
            if (s.empty()) return false;
            size_t nl = s.find_first_of(L"\r\n");
            if (nl != std::wstring::npos) s.resize(nl);
            Insert(s);
            return true;
        }
    }
    handled = false;
    return false;
}

bool TextInput::OnChar(wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F) return false;
    caretOn = true;
    Insert(std::wstring(1, ch));
    return true;
}

size_t TextInput::HitTest(IDWriteTextFormat* f, const D2D1_RECT_F& box, float x) {
    if (text.empty() || !f) return 0;
    IDWriteTextLayout* lay = nullptr;
    dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), f, 100000, box.bottom - box.top, &lay);
    if (!lay) return text.size();
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    lay->HitTestPoint(x - box.left + scrollX, (box.bottom - box.top) / 2, &trailing, &inside, &m);
    lay->Release();
    size_t pos = m.textPosition + (trailing ? m.length : 0);
    return std::min(pos, text.size());
}

void TextInput::Draw(Canvas& cv, IDWriteTextFormat* f, const D2D1_RECT_F& box, D2D1_COLOR_F textColor,
                     D2D1_COLOR_F selColor, D2D1_COLOR_F caretColor, bool focused) {
    if (!f) return;
    float h = box.bottom - box.top, w = box.right - box.left;
    IDWriteTextLayout* lay = nullptr;
    dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), f, 100000, h, &lay);
    if (!lay) return;
    lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    float cx = 0, cy = 0;
    DWRITE_HIT_TEST_METRICS m{};
    lay->HitTestTextPosition((UINT32)caret, FALSE, &cx, &cy, &m);
    // Keep the caret visible
    if (cx - scrollX > w - 2) scrollX = cx - w + 2;
    if (cx - scrollX < 0) scrollX = cx;
    DWRITE_TEXT_METRICS tm{};
    lay->GetMetrics(&tm);
    if (tm.widthIncludingTrailingWhitespace - scrollX < w - 2) scrollX = std::max(0.0f, tm.widthIncludingTrailingWhitespace - w + 2);

    cv.rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    if (focused && HasSel()) {
        UINT32 count = 0;
        lay->HitTestTextRange((UINT32)SelStart(), (UINT32)(SelEnd() - SelStart()), 0, 0, nullptr, 0, &count);
        std::vector<DWRITE_HIT_TEST_METRICS> ms(count);
        if (count && SUCCEEDED(lay->HitTestTextRange((UINT32)SelStart(), (UINT32)(SelEnd() - SelStart()), 0, 0, ms.data(), count, &count))) {
            for (auto& r : ms)
                cv.FillRound(D2D1::RectF(box.left + r.left - scrollX, box.top + h * 0.18f, box.left + r.left + r.width - scrollX,
                                         box.bottom - h * 0.18f), 2, selColor);
        }
    }
    cv.br->SetColor(textColor);
    cv.rt->DrawTextLayout(D2D1::Point2F(box.left - scrollX, box.top), lay, cv.br);
    if (focused && caretOn) {
        float x = std::floor(box.left + cx - scrollX) + 0.5f;
        cv.br->SetColor(caretColor);
        cv.rt->DrawLine(D2D1::Point2F(x, box.top + h * 0.2f), D2D1::Point2F(x, box.bottom - h * 0.2f), cv.br, 1.2f);
    }
    cv.rt->PopAxisAlignedClip();
    lay->Release();
}
