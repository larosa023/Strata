// Strata - settings, pins and session state
#pragma once
#include "common.h"

struct Pin {
    std::wstring path;
    std::wstring name;       // display name (empty = folder name)
    uint32_t color = 0;      // 0xRRGGBB color tag, 0 = none
};

// Column ids, in default display order
enum ColumnId { ColName = 0, ColSize, ColModified, ColType, ColCount };

struct Config {
    std::vector<Pin> pins;

    // Appearance (theme ids and keys match Foyer so theme files can be shared)
    std::wstring theme = L"dark";          // dark, light, midnight, glass, ember, ocean, system
    bool useWindowsAccent = true;
    uint32_t accentColor = 0x4CC2FF;       // 0xRRGGBB when not using the Windows accent
    std::wstring backdrop = L"mica";       // solid, mica, acrylic
    float opacity = 0.86f;                 // tint strength over the backdrop (0.4 - 1.0)
    float cornerRadius = 6.0f;             // 0 - 16
    std::wstring density = L"comfortable"; // compact, comfortable
    bool accentGlow = false;               // unused by Strata, kept for theme-file compatibility

    // Browsing
    bool showHidden = false;
    bool showExtensions = true;
    bool foldersFirst = true;
    float columnWidth[ColCount] = {360, 96, 150, 150};
    bool columnVisible[ColCount] = {true, true, true, true};
    int sortColumn = ColName;
    bool sortDescending = false;
    float sidebarWidth = 230;

    // Window + session
    RECT windowRect{0, 0, 0, 0};
    bool maximized = false;
    std::vector<std::wstring> sessionTabs;  // "" = This PC
    int sessionActive = 0;
    bool restoreSession = true;
};

extern Config g_config;
void ConfigLoad();
void ConfigSave();
std::wstring ConfigPath();

bool ReadUtf8File(const std::wstring& path, std::wstring& out);
bool WriteUtf8FileAtomic(const std::wstring& path, const std::wstring& text);
