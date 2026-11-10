// Strata - folder visit history: powers Jump to folder (Ctrl+G) and "What changed since my last visit"
#pragma once
#include "common.h"

void VisitRecord(const std::wstring& folder);                  // call when a folder is opened
bool VisitPrevious(const std::wstring& folder, uint64_t& unixTime);   // the visit before this one (0 if none)
void VisitForget(const std::wstring& folder);
void VisitsSave(bool force = false);

struct JumpHit {
    std::wstring path;
    float score = 0;
    bool pinned = false;
};
// Fuzzy, frecency-ranked folder search: "smi bid" finds D:\Bids\2026\Smith Library.
// `extra` are always-candidates (pins, drives) that may never have been visited.
std::vector<JumpHit> JumpQuery(const std::wstring& query, const std::vector<std::wstring>& pins, const std::vector<std::wstring>& extra, size_t max);
// Character ranges in `path` that matched (for highlighting)
std::vector<std::pair<size_t, size_t>> JumpHighlights(const std::wstring& path, const std::wstring& query);
