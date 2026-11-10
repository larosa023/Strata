// Strata - search query parsing shared by every search engine
//   words (all must match the name), *.pdf / report?.docx wildcards, and filters:
//   type:pdf  type:pdf,docx  size:>50mb  size:<1kb  modified:7d  modified:2026  tag:urgent  folder:  file:
#pragma once
#include "common.h"

struct SearchQuery {
    std::vector<std::wstring> words;      // lowercase name terms (may contain * ?)
    std::vector<std::wstring> exts;       // lowercase extensions without the dot
    uint64_t minSize = 0, maxSize = UINT64_MAX;
    uint64_t modifiedAfter = 0;           // FILETIME as integer; 0 = any
    uint64_t modifiedBefore = UINT64_MAX;
    std::wstring tag;                     // tag name or id (checked on the UI thread)
    bool foldersOnly = false, filesOnly = false;
    bool Empty() const { return words.empty() && exts.empty() && minSize == 0 && maxSize == UINT64_MAX && !modifiedAfter && tag.empty() && !foldersOnly && !filesOnly; }
    bool Match(const std::wstring& nameLower, bool isDir, uint64_t size, const FILETIME& modified) const;
    std::wstring ToEverything() const;    // the same query in Everything's syntax (tag excluded)
};

SearchQuery ParseQuery(const std::wstring& text);
