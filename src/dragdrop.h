// Strata - drag and drop (Strata as a drop target, and dragging files out of Strata)
#pragma once
#include "common.h"
#include <functional>
#include <ole2.h>

struct DropHandlers {
    // Return the folder a drop at this client point (pixels) would land in, or "" for none.
    std::function<std::wstring(POINT clientPt)> targetAt;
    // Highlight feedback while dragging (folder may be "")
    std::function<void(const std::wstring& folder)> hover;
    // Perform the drop: copy or move these paths into the folder
    std::function<void(const std::vector<std::wstring>& paths, const std::wstring& folder, DWORD effect)> drop;
};

bool DragDropRegister(HWND hwnd, const DropHandlers& handlers);
void DragDropRevoke(HWND hwnd);
// Start dragging these files out (blocks until the drag ends). Returns the effect performed.
DWORD DragFiles(HWND hwnd, const std::vector<std::wstring>& paths);
bool DragInProgressFromUs();
