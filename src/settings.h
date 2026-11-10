// Strata - settings window (custom drawn like Foyer's; changes apply live to the main window)
#pragma once
#include "common.h"

enum SettingsPage { PageGeneral = 0, PageLayout, PageThemes, PageSearch, PageTags, PageFileOps, PageUpdates, PageAbout, PageCount };
void SettingsOpen(int page = -1);
bool SettingsIsOpen();
void SettingsOnUpdateStatus();
void SettingsClose();

// Small modal text prompt (rename a tag, name a saved search, ...)
bool PromptText(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value);

// Implemented by the main window: re-apply settings after a change
void AppSettingsChanged();
HWND AppWindow();
