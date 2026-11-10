// Strata - version stacking: Plans_rev1.pdf, Plans_rev2.pdf, Plans rev 3.pdf, Plans (2).pdf, Plans - Copy.pdf,
// Plans_2026-03-01.pdf … are versions of "Plans.pdf" and can collapse into one row showing the latest.
#pragma once
#include "common.h"

struct VersionKey {
    std::wstring key;      // group key: lowercase stem + extension
    double rank = -1;      // higher = later (-1: no version marker, e.g. the original "Plans.pdf")
    bool marker = false;   // the name carries a version marker
};

VersionKey ParseVersion(const std::wstring& name, bool isDir);
// Short label for a version, e.g. "rev 3", "v2", "2026-03-01", "copy" ("" for the original)
std::wstring VersionLabel(const std::wstring& name, bool isDir);
