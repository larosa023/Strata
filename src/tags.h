// Strata - color tags on files and folders (stored in tags.json, keyed by path)
#pragma once
#include "common.h"
#include "json.h"

struct TagDef;
void TagsLoad();
std::wstring TagOf(const std::wstring& path);             // tag id, or ""
void TagSet(const std::wstring& path, const std::wstring& tagId);   // "" removes
void TagsPathMoved(const std::wstring& from, const std::wstring& to);   // keeps tags after rename / move
std::vector<std::wstring> TagsFind(const std::wstring& tagId);
void TagsRemoveTag(const std::wstring& tagId);             // a tag was deleted in Settings
const TagDef* TagDefById(const std::wstring& id);
uint32_t TagColor(const std::wstring& id);                 // 0 when none
json::Value TagsToJson();
void TagsFromJson(const json::Value& v);
