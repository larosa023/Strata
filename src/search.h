// Strata - search across folders and drives
//
// Two engines:
//  * Everything (voidtools), when it's running: instant results over every drive through its IPC window.
//  * A built-in background scan when it isn't: walks the folders at low priority and streams results.
// Phase 3 adds Strata's own lightweight index.
#pragma once
#include "fs.h"

struct SearchBatch {
    int tab = 0;
    uint64_t gen = 0;
    std::vector<Item> items;           // Item::path holds the full path; Item::type indexes typeNames
    std::vector<std::wstring> typeNames;
    bool done = false;                 // last batch for this search
    bool capped = false;               // stopped at the result limit
    bool everything = false;           // results came from Everything
    uint64_t total = 0;                // Everything: total matches (may exceed what was returned)
    size_t scannedFolders = 0;         // built-in scan progress
    bool fromIndex = false;            // results came from Strata's own index
};

constexpr size_t kSearchMaxResults = 5000;

// True when the Everything search engine is running and can answer queries.
bool EverythingAvailable();

// Start a search for `query` under `roots` (empty roots = every drive). Results arrive as
// WM_STRATA_SEARCH messages carrying SearchBatch*. A newer search or listing for the same tab cancels this one.
void SearchAsync(HWND notify, int tab, uint64_t gen, const std::vector<std::wstring>& roots, const std::wstring& query);

// Everything replies arrive on the UI thread as WM_COPYDATA; returns true if this message was one.
bool SearchHandleCopyData(HWND notify, const COPYDATASTRUCT* cds);
