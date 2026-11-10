// Strata - Windows shell integration: icons, context menus, file operations, clipboard
#pragma once
#include "common.h"
#include <d2d1.h>
#include <functional>

// ---- icons (system image list -> Direct2D bitmaps, cached per render target)
void IconsInit(bool large);                      // large = 32px source images (high DPI)
ID2D1Bitmap* IconBitmap(ID2D1RenderTarget* rt, int index);
void IconsDiscard();                             // render target lost / DPI changed

// ---- file operations (run on a worker thread using the Windows copy engine)
enum class OpKind { Copy, Move, Recycle, Delete };
struct OpResult {
    OpKind kind;
    bool ok = false;
    bool aborted = false;
    HRESULT hr = S_OK;
    std::wstring dest;
    std::vector<std::wstring> sources;
};
void FileOpAsync(HWND owner, OpKind kind, const std::vector<std::wstring>& sources, const std::wstring& destFolder);
bool RenameItem(HWND owner, const std::wstring& path, const std::wstring& newName, std::wstring* error);
std::wstring UniqueName(const std::wstring& folder, const std::wstring& base, const std::wstring& ext);

// ---- clipboard (interoperates with File Explorer)
bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths, bool cut);
bool ClipboardGetFiles(std::vector<std::wstring>& paths, bool& cut);
bool ClipboardHasFiles();
void ClipboardSetText(HWND owner, const std::wstring& text);

// ---- launching
// Opens a file with its default program. If the item is a shortcut to a folder, returns false and sets *folderTarget.
bool OpenItem(HWND owner, const std::wstring& path, std::wstring* folderTarget);
void OpenTerminal(const std::wstring& folder);
void ShowProperties(HWND owner, const std::vector<std::wstring>& paths);

// ---- context menu
// Shows the Windows context menu for the given items with Strata's own entries on top.
// `extra` lists (id, label) pairs; id 0 is a separator. Returns the chosen extra id (>0),
// 0 if nothing / a shell command ran, or -1 if the shell verb was intercepted via `onVerb`.
struct MenuEntry { int id; std::wstring label; bool checked = false; bool disabled = false; };
int ShowItemContextMenu(HWND owner, POINT screenPt, const std::wstring& folder, const std::vector<std::wstring>& paths,
                        const std::vector<MenuEntry>& extra,
                        const std::function<bool(const std::wstring& verb)>& onVerb);
// Lets submenus such as "Send to" and "Open with" draw while a shell menu is open.
bool ShellMenuHandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
