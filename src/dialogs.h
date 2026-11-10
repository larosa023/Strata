// Strata - themed dialogs
//
// Every question Strata asks (conflicts, confirmations, update notes, names) is drawn with the
// current theme instead of plain Windows message boxes. ThemeWin32Dialog() dresses a classic
// dialog-template dialog (batch rename) in the theme's colors.
#pragma once
#include "common.h"

struct DlgButton {
    int id = 0;
    std::wstring label;
    bool primary = false;   // accent-colored, the default for Enter
    bool danger = false;    // red (delete permanently, …)
};

struct DlgSpec {
    std::wstring heading;            // the question, in bold
    std::wstring body;               // supporting text (\n for new lines; long text scrolls)
    enum Icon { None, Info, Warning, Question, Danger, Update } icon = None;
    std::vector<DlgButton> buttons;  // left to right
    int cancelId = 0;                // what Esc / closing means
    std::wstring checkbox;           // optional "Do this for all…" box
    bool checked = false;
    bool input = false;              // show a text box
    std::wstring inputText;
    std::wstring inputHint;          // grey placeholder
    float width = 440;               // DIPs (grows for wide button rows)
};

struct DlgResult {
    int id = 0;
    bool checked = false;
    std::wstring text;
};

DlgResult ShowDialog(HWND owner, const DlgSpec& spec);

// Shortcuts
bool ConfirmDialog(HWND owner, const std::wstring& heading, const std::wstring& body, const std::wstring& okLabel,
                   bool danger = false, DlgSpec::Icon icon = DlgSpec::Question);
void NoticeDialog(HWND owner, const std::wstring& heading, const std::wstring& body, bool warning = false);
bool TextDialog(HWND owner, const std::wstring& heading, const std::wstring& body, std::wstring& value,
                const std::wstring& okLabel = L"OK");

// Applies the theme's colors to a dialog built from a DLGTEMPLATE (call in WM_INITDIALOG).
void ThemeWin32Dialog(HWND dlg);
