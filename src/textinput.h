// Strata - small self-drawn single-line text field (search, path bar, rename)
#pragma once
#include "common.h"
#include "gfx.h"

struct TextInput {
    std::wstring text;
    size_t caret = 0;
    size_t anchor = 0;      // selection is [min(anchor,caret), max(anchor,caret))
    float scrollX = 0;      // horizontal scroll when the text is wider than the box
    bool caretOn = true;

    size_t SelStart() const { return anchor < caret ? anchor : caret; }
    size_t SelEnd() const { return anchor < caret ? caret : anchor; }
    bool HasSel() const { return anchor != caret; }
    void SetText(const std::wstring& s) { text = s; caret = anchor = s.size(); scrollX = 0; }
    void SelectAll() { anchor = 0; caret = text.size(); }
    void Select(size_t a, size_t b) { anchor = a; caret = b; }

    // Returns true if the text changed. `handled` says whether the key was consumed.
    bool OnKey(HWND hwnd, UINT vk, bool& handled);
    bool OnChar(wchar_t ch);
    void Draw(gfx::Canvas& cv, IDWriteTextFormat* f, const D2D1_RECT_F& box, D2D1_COLOR_F textColor,
              D2D1_COLOR_F selColor, D2D1_COLOR_F caretColor, bool focused);
    size_t HitTest(IDWriteTextFormat* f, const D2D1_RECT_F& box, float x);

private:
    void Insert(const std::wstring& s);
    bool DeleteSel();
    size_t WordLeft(size_t i) const;
    size_t WordRight(size_t i) const;
};
