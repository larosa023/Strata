// Strata - settings, pins, tags, saved searches and session state
#pragma once
#include "common.h"

struct Pin {
    std::wstring path;
    std::wstring name;       // display name (empty = folder name)
    uint32_t color = 0;      // 0xRRGGBB color tag, 0 = none
};

struct TagDef {
    std::wstring id;         // stable id ("urgent", "t1699999")
    std::wstring name;       // display name
    uint32_t color = 0;      // 0xRRGGBB
};

struct SavedSearch {
    std::wstring name;
    std::wstring root;       // "" = everywhere
    std::wstring query;
};

struct SessionTab {
    std::wstring left, right;
    int activePane = 0;
};

// Column ids, in default display order
enum ColumnId { ColName = 0, ColSize, ColModified, ColType, ColCount };

struct Config {
    std::vector<Pin> pins;
    std::vector<TagDef> tags;
    std::vector<SavedSearch> savedSearches;

    // ---- Appearance (keys shared with Foyer so theme files work in both)
    std::wstring theme = L"dark";          // dark, light, midnight, glass, ember, ocean, system, custom
    bool useWindowsAccent = true;
    uint32_t accentColor = 0x4CC2FF;       // 0xRRGGBB when not using the Windows accent
    std::wstring backdrop = L"mica";       // solid, mica, acrylic
    float opacity = 0.86f;                 // tint strength over the backdrop (0.4 - 1.0)
    float cornerRadius = 6.0f;             // 0 - 16
    bool accentGlow = false;               // Foyer key; unused by Strata
    std::wstring customTheme;              // name of the imported theme when theme == custom
    std::vector<std::pair<std::wstring, uint32_t>> customColors;   // color overrides from a theme file
    std::wstring fontFamily;               // "" = Segoe UI Variable / Segoe UI
    float fontSize = 13.0f;                // 11 - 16
    std::wstring density = L"comfortable"; // compact, comfortable
    std::wstring iconSize = L"medium";     // small, medium, large (list icons)
    std::wstring animation = L"smooth";    // none, fast, smooth
    bool respectHighContrast = true;

    // ---- Layout
    std::wstring layout = L"dual";         // dual, single
    float paneRatio = 0.5f;
    bool showSidebar = true;
    bool showPreview = false;
    bool showStatusBar = true;
    bool showNavButtons = true;            // back / forward / up / refresh
    bool showViewSwitch = true;
    bool sidebarRecent = true;
    bool sidebarTags = true;
    bool sidebarSaved = true;
    float sidebarWidth = 230;
    float previewWidth = 320;

    // ---- Browsing
    bool showHidden = false;
    bool showExtensions = true;
    bool foldersFirst = true;
    bool rememberFolderViews = true;
    float columnWidth[ColCount] = {360, 96, 150, 150};
    bool columnVisible[ColCount] = {true, true, true, true};
    int sortColumn = ColName;
    bool sortDescending = false;
    std::vector<std::wstring> recentFolders;
    std::vector<std::wstring> sidebarOrder;       // section ids: pinned, drives, tags, saved, recent (dragged into place)
    std::vector<std::wstring> sidebarCollapsed;   // collapsed section ids
    std::vector<std::wstring> driveOrder;         // drive roots in the order the user dragged them (others follow)

    // ---- Folder sizes
    bool folderSizes = true;
    bool folderSizesNetwork = false;

    // ---- File operations
    bool confirmDelete = false;            // Recycle Bin deletes are undoable, so no prompt by default
    bool verifyCopies = false;
    std::wstring conflictDefault = L"ask"; // ask, skip, replace, keepboth
    bool skipErrors = false;

    // ---- Search
    bool useEverything = true;
    bool useIndex = false;                 // Strata's own whole-PC index
    bool searchNetwork = false;

    // ---- Context menu
    std::vector<std::wstring> hiddenMenuItems;   // labels (without &) of Windows menu entries to hide
    std::vector<std::wstring> seenMenuItems;     // entries seen recently, offered in Settings

    // ---- System
    bool keepInTray = false;               // closing the window keeps Strata ready in the tray
    bool defaultFileManager = false;       // mirrors the registry state

    // ---- Updates
    bool autoCheck = true;
    bool autoInstall = false;
    std::wstring channel = L"stable";      // stable, beta
    int checkHours = 6;
    double lastCheck = 0;                  // unix seconds
    std::wstring notifiedVersion;

    // ---- Window + session
    RECT windowRect{0, 0, 0, 0};
    bool maximized = false;
    std::vector<SessionTab> session;
    int sessionActive = 0;
    bool restoreSession = true;
};

extern Config g_config;
void ConfigLoad();
void ConfigSave();
std::wstring ConfigPath();
void ConfigDefaultTags();

// Backup/restore everything (settings, pins, tags, saved searches, folder views, tag assignments) to one file
bool ConfigBackupTo(const std::wstring& path, std::wstring* error);
bool ConfigRestoreFrom(const std::wstring& path, std::wstring* error);

// Theme files (*.theme.json) shared across the suite
bool ThemeExport(const std::wstring& path, const std::wstring& name);
bool ThemeImport(const std::wstring& path, std::wstring* error);

// Per-folder view memory
struct FolderView { int sortColumn = ColName; bool sortDescending = false; int viewMode = 0; };
bool FolderViewGet(const std::wstring& folder, FolderView& out);
void FolderViewSet(const std::wstring& folder, const FolderView& v);

bool ReadUtf8File(const std::wstring& path, std::wstring& out);
bool WriteUtf8FileAtomic(const std::wstring& path, const std::wstring& text);
uint32_t ParseHexColor(const std::wstring& s, uint32_t def);
std::wstring HexColor(uint32_t c);
