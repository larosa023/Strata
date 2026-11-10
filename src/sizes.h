// Strata - folder sizes calculated in the background (low CPU and I/O priority, pauses during games)
#pragma once
#include "common.h"

struct FolderSize {
    uint64_t bytes = 0;
    uint64_t files = 0, folders = 0;
    bool done = false;       // false while still being calculated
};

void SizesInit(HWND notify);
// Ask for these folders (front of the queue). `force` recalculates even if cached.
void SizesRequest(const std::vector<std::wstring>& folders, bool force = false);
void SizesCancelPending();   // drop queued requests (not the cache) - e.g. after navigating
bool SizesGet(const std::wstring& folder, FolderSize& out);
void SizesInvalidate(const std::wstring& path);   // a folder changed: forget it and its parents
bool SizesAllowed(const std::wstring& folder);    // false for network folders unless enabled
