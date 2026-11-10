// Strata - background folder enumeration, drives, folder watching
#pragma once
#include "common.h"
#include <memory>

struct Item {
    std::wstring name;
    uint64_t size = 0;
    FILETIME modified{};
    DWORD attrs = 0;
    int type = 0;        // index into Listing::types
    int icon = -1;       // system image list index
    bool isDir() const { return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    // Drive rows on the This PC page
    uint64_t total = 0, freeBytes = 0;
    std::wstring path;   // full path for drive rows; empty for normal items
};

struct Listing {
    int tab = 0;                     // tab id that asked
    uint64_t gen = 0;                // request generation (stale results are dropped)
    std::wstring folder;             // "" = This PC
    std::vector<Item> items;
    std::vector<std::wstring> types; // type names ("File folder", "PDF Document", ...)
    DWORD error = 0;                 // Win32 error if the folder couldn't be read
    double ms = 0;                   // time taken
};

struct IconBatch {
    int tab = 0;
    uint64_t gen = 0;
    std::vector<std::pair<int, int>> icons;   // (item index in Listing order, icon index)
};

struct DriveInfo {
    std::wstring root;     // "C:\"
    std::wstring label;    // "Local Disk"
    UINT type = 0;         // DRIVE_FIXED ...
    uint64_t total = 0, freeBytes = 0;
    int icon = -1;
    bool ready = false;
};

// Enumerate a folder on a worker thread; posts WM_STRATA_LISTING (and later WM_STRATA_ICONS) to notify.
void ListFolderAsync(HWND notify, int tab, uint64_t gen, const std::wstring& folder);
// Enumerate drives on a worker thread; posts WM_STRATA_DRIVES.
void ListDrivesAsync(HWND notify);
// Drive display name, e.g. "Local Disk (C:)"
std::wstring DriveDisplayName(const DriveInfo& d);

// Watch one folder for changes; posts WM_STRATA_DIRCHANGED(gen). Empty path stops watching.
void WatchFolder(HWND notify, const std::wstring& folder);
uint64_t WatchGeneration();

// Path helpers
std::wstring PathJoin(const std::wstring& dir, const std::wstring& name);
std::wstring PathParent(const std::wstring& path);      // "" for a drive root (This PC)
std::wstring PathLeaf(const std::wstring& path);        // folder display name
std::wstring NormalizeFolder(const std::wstring& path); // trims trailing slashes except on roots
bool IsDirectory(const std::wstring& path);
std::vector<std::wstring> SplitPath(const std::wstring& path, std::vector<std::wstring>* fullPaths);
std::vector<std::wstring> ListSubfolders(const std::wstring& folder, size_t max, bool includeHidden);
int FolderIconIndex();
void TypeAndIcon(const std::wstring& name, bool dir, std::wstring& typeName, int& icon);
// Generation tracking shared by folder listings and searches (a newer request for a tab cancels older ones)
void MarkLatest(int tab, uint64_t gen);
bool Superseded(int tab, uint64_t gen);
std::wstring FormatSize(uint64_t bytes);
std::wstring FormatDate(const FILETIME& ft);
