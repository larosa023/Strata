// Strata - background loaders for the preview panel and grid thumbnails, plus hosting of
// Windows preview handlers (PDF, Office, ...)
#pragma once
#include "common.h"

struct PixelImage {
    UINT w = 0, h = 0;
    std::vector<BYTE> pixels;    // 32bpp premultiplied BGRA, stride w*4
};

struct ThumbResult {
    std::wstring path;
    int px = 0;
    PixelImage img;
    bool ok = false;
};

struct PreviewResult {
    uint64_t gen = 0;
    std::wstring path;
    enum Kind { None, Image, Text, Thumb, Folder, Handler } kind = None;
    PixelImage img;
    UINT imgW = 0, imgH = 0;       // original image size
    std::wstring text;             // Text: the first part of the file
    bool truncated = false;
    uint64_t files = 0, folders = 0;   // Folder: direct children
    std::wstring handlerClsid;     // Handler: a Windows preview handler can show this file
};

struct GitResult {
    uint64_t gen = 0;
    std::wstring folder;           // folder that was asked about
    bool isRepo = false;
    std::wstring root, branch;
    int changed = -1, untracked = 0, ahead = 0, behind = 0;   // changed = -1 when git.exe isn't available
};

void ThumbRequest(HWND notify, const std::wstring& path, int px);
void ThumbCancelAll();
void PreviewLoadAsync(HWND notify, uint64_t gen, const std::wstring& path, bool isDir, int maxPx);
void GitInfoAsync(HWND notify, uint64_t gen, const std::wstring& folder);

// Preview handler host (child window over the preview area)
bool PreviewHandlerShow(HWND parent, const std::wstring& path, const std::wstring& clsid, RECT rc, COLORREF bg, COLORREF fg);
void PreviewHandlerMove(RECT rc);
void PreviewHandlerClose();
bool PreviewHandlerActive();

bool IsTextFile(const std::wstring& path);
bool IsImageFile(const std::wstring& path);
