// Strata - its own lightweight whole-PC file index (optional; used when Everything isn't running)
#pragma once
#include "common.h"
#include "fs.h"
#include "query.h"

void IndexInit(HWND notify);
void IndexApplySettings();           // starts / stops based on g_config.useIndex
bool IndexReady();
std::wstring IndexStatusText();
// Search the index. roots empty = everything. Returns false if the index isn't ready.
bool IndexQuery(const std::vector<std::wstring>& roots, const SearchQuery& q, size_t max, std::vector<Item>& out, bool& capped);
