// Strata - preview + thumbnail loaders, Git status, preview handler host
#include "preview.h"
#include "fs.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static std::wstring ExtOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L'\\');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    return ToLower(path.substr(dot));
}

bool IsImageFile(const std::wstring& path) {
    static const std::set<std::wstring> k = {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff", L".ico", L".webp",
                                             L".heic", L".heif", L".jxr", L".wdp", L".dds", L".avif", L".jfif"};
    return k.count(ExtOf(path)) != 0;
}

bool IsTextFile(const std::wstring& path) {
    static const std::set<std::wstring> k = {
        L".txt", L".md", L".markdown", L".log", L".json", L".jsonc", L".xml", L".yml", L".yaml", L".ini", L".cfg", L".conf", L".csv",
        L".tsv", L".c", L".cc", L".cpp", L".cxx", L".h", L".hpp", L".hh", L".cs", L".js", L".mjs", L".cjs", L".ts", L".jsx",
        L".tsx", L".py", L".rb", L".go", L".rs", L".java", L".kt", L".swift", L".php", L".html", L".htm", L".css", L".scss",
        L".less", L".sql", L".sh", L".bash", L".zsh", L".ps1", L".psm1", L".bat", L".cmd", L".toml", L".gitignore",
        L".gitattributes", L".editorconfig", L".env", L".lua", L".r", L".m", L".mm", L".vb", L".fs", L".dart", L".vue",
        L".svelte", L".gradle", L".properties", L".reg", L".nfo", L".srt", L".vtt", L".tex", L".rst", L".adoc", L".cmake",
        L".mk", L".makefile", L".dockerfile", L".gd", L".glsl", L".hlsl", L".shader", L".asm", L".s", L".pl", L".diff", L".patch"};
    std::wstring e = ExtOf(path);
    if (k.count(e)) return true;
    std::wstring leaf = ToLower(PathLeaf(path));
    return leaf == L"makefile" || leaf == L"dockerfile" || leaf == L"license" || leaf == L"readme" || leaf == L"cmakelists.txt";
}

// ---------------------------------------------------------------- WIC helpers
static IWICImagingFactory* Wic() {
    thread_local IWICImagingFactory* f = nullptr;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

static bool SourceToPixels(IWICBitmapSource* src, PixelImage& out, int maxPx) {
    IWICImagingFactory* wic = Wic();
    if (!wic || !src) return false;
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    if (!w || !h) return false;
    IWICBitmapSource* cur = src;
    cur->AddRef();
    if (maxPx > 0 && (w > (UINT)maxPx || h > (UINT)maxPx)) {
        double k = std::min((double)maxPx / w, (double)maxPx / h);
        UINT nw = std::max(1u, (UINT)(w * k)), nh = std::max(1u, (UINT)(h * k));
        IWICBitmapScaler* sc = nullptr;
        if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(cur, nw, nh, WICBitmapInterpolationModeFant))) {
            cur->Release();
            cur = sc;
            w = nw;
            h = nh;
        } else {
            Rel(sc);
        }
    }
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(cur, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) {
        out.w = w;
        out.h = h;
        out.pixels.resize((size_t)w * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.pixels.size(), out.pixels.data()));
    }
    Rel(conv);
    cur->Release();
    return ok;
}

static bool DecodeImage(const std::wstring& path, PixelImage& out, int maxPx, UINT* ow, UINT* oh) {
    IWICImagingFactory* wic = Wic();
    if (!wic) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame))) {
        frame->GetSize(ow, oh);
        // honor EXIF orientation for photos
        IWICMetadataQueryReader* meta = nullptr;
        IWICBitmapSource* src = frame;
        src->AddRef();
        if (SUCCEEDED(frame->GetMetadataQueryReader(&meta)) && meta) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(meta->GetMetadataByName(L"/app1/ifd/{ushort=274}", &pv)) && pv.vt == VT_UI2 && pv.uiVal > 1) {
                WICBitmapTransformOptions opt = WICBitmapTransformRotate0;
                switch (pv.uiVal) {
                    case 3: opt = WICBitmapTransformRotate180; break;
                    case 6: opt = WICBitmapTransformRotate90; break;
                    case 8: opt = WICBitmapTransformRotate270; break;
                    case 2: opt = WICBitmapTransformFlipHorizontal; break;
                    case 4: opt = WICBitmapTransformFlipVertical; break;
                    default: break;
                }
                IWICBitmapFlipRotator* rot = nullptr;
                if (opt != WICBitmapTransformRotate0 && SUCCEEDED(wic->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(frame, opt))) {
                    src->Release();
                    src = rot;
                    if (opt == WICBitmapTransformRotate90 || opt == WICBitmapTransformRotate270) std::swap(*ow, *oh);
                } else {
                    Rel(rot);
                }
            }
            PropVariantClear(&pv);
            meta->Release();
        }
        ok = SourceToPixels(src, out, maxPx);
        src->Release();
    }
    Rel(frame);
    Rel(dec);
    return ok;
}

static bool ShellThumb(const std::wstring& path, int px, PixelImage& out, bool thumbOnly) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP hb = nullptr;
    SIIGBF flags = SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT;
    if (thumbOnly) flags |= SIIGBF_THUMBNAILONLY;
    bool ok = false;
    if (SUCCEEDED(f->GetImage(SIZE{px, px}, flags, &hb)) && hb) {
        IWICBitmap* wb = nullptr;
        if (Wic() && SUCCEEDED(Wic()->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, &wb))) {
            ok = SourceToPixels(wb, out, px);
            wb->Release();
        }
        DeleteObject(hb);
    }
    f->Release();
    return ok;
}

// ---------------------------------------------------------------- thumbnail workers
static std::mutex g_tLock;
static std::condition_variable g_tCv;
static std::deque<std::pair<std::wstring, int>> g_tQueue;
static std::set<std::wstring> g_tPending;
static HWND g_tNotify = nullptr;
static bool g_tStarted = false;

static void ThumbWorker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::pair<std::wstring, int> job;
        {
            std::unique_lock<std::mutex> l(g_tLock);
            g_tCv.wait(l, [] { return !g_tQueue.empty(); });
            job = g_tQueue.front();   // newest first (the user is probably looking at it)
            g_tQueue.pop_front();
        }
        auto* r = new ThumbResult;
        r->path = job.first;
        r->px = job.second;
        r->ok = ShellThumb(job.first, job.second, r->img, false);
        {
            std::lock_guard<std::mutex> l(g_tLock);
            g_tPending.erase(ToLower(job.first));
        }
        if (!PostMessageW(g_tNotify, WM_STRATA_THUMB, 0, (LPARAM)r)) delete r;
    }
}

void ThumbRequest(HWND notify, const std::wstring& path, int px) {
    std::lock_guard<std::mutex> l(g_tLock);
    g_tNotify = notify;
    if (!g_tStarted) {
        g_tStarted = true;
        std::thread(ThumbWorker).detach();
        std::thread(ThumbWorker).detach();
    }
    std::wstring key = ToLower(path);
    if (g_tPending.count(key)) return;
    g_tPending.insert(key);
    g_tQueue.push_front({path, px});
    while (g_tQueue.size() > 200) {
        g_tPending.erase(ToLower(g_tQueue.back().first));
        g_tQueue.pop_back();
    }
    g_tCv.notify_one();
}

void ThumbCancelAll() {
    std::lock_guard<std::mutex> l(g_tLock);
    for (auto& q : g_tQueue) g_tPending.erase(ToLower(q.first));
    g_tQueue.clear();
}

// ---------------------------------------------------------------- preview loader
static std::wstring HandlerFor(const std::wstring& path) {
    std::wstring ext = ExtOf(path);
    if (ext.empty()) return L"";
    wchar_t buf[64] = {};
    DWORD len = 64;
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, ext.c_str(),
                                    L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", buf, &len)))
        return buf;
    return L"";
}

static std::wstring DecodeText(const std::string& raw, bool& truncated) {
    std::wstring out;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        out.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
    } else if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        for (size_t i = 2; i + 1 < raw.size(); i += 2) out += (wchar_t)(((unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1]);
    } else {
        size_t skip = (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) ? 3 : 0;
        // cut at a UTF-8 boundary
        size_t n = raw.size();
        while (truncated && n > skip && ((unsigned char)raw[n - 1] & 0xC0) == 0x80) --n;
        if (truncated && n > skip && ((unsigned char)raw[n - 1] & 0x80)) --n;
        int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data() + skip, (int)(n - skip), nullptr, 0);
        UINT cp = wn > 0 ? CP_UTF8 : CP_ACP;
        DWORD fl = wn > 0 ? MB_ERR_INVALID_CHARS : 0;
        wn = MultiByteToWideChar(cp, fl, raw.data() + skip, (int)(n - skip), nullptr, 0);
        out.assign(std::max(0, wn), L'\0');
        if (wn > 0) MultiByteToWideChar(cp, fl, raw.data() + skip, (int)(n - skip), &out[0], wn);
    }
    // tabs to spaces, drop control chars
    std::wstring clean;
    clean.reserve(out.size());
    for (wchar_t c : out) {
        if (c == L'\t') clean += L"    ";
        else if (c == L'\r') continue;
        else if (c < 0x20 && c != L'\n') clean += L'\x00B7';
        else clean += c;
    }
    return clean;
}

static bool LooksBinary(const std::string& s) {
    size_t zeros = 0, n = std::min<size_t>(s.size(), 4096);
    for (size_t i = 0; i < n; ++i) if (s[i] == 0) ++zeros;
    return zeros > 0 && !(s.size() >= 2 && ((unsigned char)s[0] == 0xFF || (unsigned char)s[0] == 0xFE));
}

void PreviewLoadAsync(HWND notify, uint64_t gen, const std::wstring& path, bool isDir, int maxPx) {
    std::thread([=] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto* r = new PreviewResult;
        r->gen = gen;
        r->path = path;
        if (isDir) {
            r->kind = PreviewResult::Folder;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW(PathJoin(path, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) r->folders++; else r->files++;
                } while (FindNextFileW(h, &fd) && r->files + r->folders < 1000000);
                FindClose(h);
            }
        } else if (IsImageFile(path) && DecodeImage(path, r->img, maxPx, &r->imgW, &r->imgH)) {
            r->kind = PreviewResult::Image;
        } else {
            bool text = IsTextFile(path);
            std::string raw;
            if (!text) {
                std::wstring h = HandlerFor(path);
                if (!h.empty()) { r->kind = PreviewResult::Handler; r->handlerClsid = h; }
            }
            if (r->kind == PreviewResult::None) {
                HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                LARGE_INTEGER sz{};
                if (fh != INVALID_HANDLE_VALUE) {
                    GetFileSizeEx(fh, &sz);
                    if (text || sz.QuadPart < 2 * 1024 * 1024) {
                        const DWORD cap = 96 * 1024;
                        raw.resize((size_t)std::min<LONGLONG>(sz.QuadPart, cap));
                        DWORD read = 0;
                        if (!raw.empty()) ReadFile(fh, &raw[0], (DWORD)raw.size(), &read, nullptr);
                        raw.resize(read);
                        r->truncated = sz.QuadPart > cap;
                    }
                    CloseHandle(fh);
                }
                if (!raw.empty() && (text || !LooksBinary(raw)) && (text || ExtOf(path).empty())) {
                    r->kind = PreviewResult::Text;
                    r->text = DecodeText(raw, r->truncated);
                } else if (ShellThumb(path, maxPx, r->img, true)) {
                    r->kind = PreviewResult::Thumb;   // videos, documents with thumbnails, ...
                }
            }
        }
        if (!PostMessageW(notify, WM_STRATA_PREVIEW, 0, (LPARAM)r)) delete r;
        CoUninitialize();
    }).detach();
}

// ---------------------------------------------------------------- git
static bool FindRepoRoot(const std::wstring& start, std::wstring& root, std::wstring& gitDir) {
    std::wstring p = start;
    for (int depth = 0; depth < 40 && !p.empty(); ++depth) {
        std::wstring g = PathJoin(p, L".git");
        DWORD a = GetFileAttributesW(g.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) {
            root = p;
            if (a & FILE_ATTRIBUTE_DIRECTORY) { gitDir = g; return true; }
            // worktree / submodule: ".git" is a file pointing at the real git dir
            std::string txt;
            HANDLE h = CreateFileW(g.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                char buf[1024];
                DWORD n = 0;
                ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
                txt.assign(buf, n);
                CloseHandle(h);
            }
            size_t k = txt.find("gitdir:");
            if (k == std::string::npos) return false;
            std::string d = txt.substr(k + 7);
            while (!d.empty() && (d.front() == ' ')) d.erase(d.begin());
            while (!d.empty() && (d.back() == '\n' || d.back() == '\r' || d.back() == ' ')) d.pop_back();
            std::wstring wd(d.begin(), d.end());
            for (auto& c : wd) if (c == L'/') c = L'\\';
            gitDir = (wd.size() > 1 && wd[1] == L':') ? wd : PathJoin(p, wd);
            return true;
        }
        std::wstring parent = PathParent(p);
        if (parent == p) break;
        p = parent;
    }
    return false;
}

static std::wstring ReadHeadBranch(const std::wstring& gitDir) {
    HANDLE h = CreateFileW(PathJoin(gitDir, L"HEAD").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    char buf[512];
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.rfind("ref: refs/heads/", 0) == 0) s = s.substr(16);
    else if (s.size() >= 7) s = s.substr(0, 7);   // detached HEAD: short hash
    return std::wstring(s.begin(), s.end());
}

static bool RunGitStatus(const std::wstring& root, std::string& out) {
    wchar_t git[MAX_PATH];
    if (!SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, git, nullptr)) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + std::wstring(git) + L"\" -C \"" + root + L"\" status --porcelain=v1 -b --untracked-files=normal";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    bool ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi) != FALSE;
    CloseHandle(wr);
    if (ok) {
        char chunk[4096];
        DWORD n = 0;
        DWORD start = GetTickCount();
        while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n) {
            out.append(chunk, n);
            if (out.size() > 4 * 1024 * 1024 || GetTickCount() - start > 8000) break;
        }
        if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        ok = code == 0;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return ok;
}

void GitInfoAsync(HWND notify, uint64_t gen, const std::wstring& folder) {
    std::thread([=] {
        auto* r = new GitResult;
        r->gen = gen;
        r->folder = folder;
        std::wstring gitDir;
        if (!folder.empty() && folder.rfind(L"\\\\", 0) != 0 && FindRepoRoot(folder, r->root, gitDir)) {
            r->isRepo = true;
            r->branch = ReadHeadBranch(gitDir);
            std::string out;
            if (RunGitStatus(r->root, out)) {
                r->changed = 0;
                size_t i = 0;
                while (i < out.size()) {
                    size_t nl = out.find('\n', i);
                    std::string line = out.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
                    i = nl == std::string::npos ? out.size() : nl + 1;
                    if (line.rfind("## ", 0) == 0) {
                        size_t a = line.find("ahead "), b = line.find("behind ");
                        if (a != std::string::npos) r->ahead = atoi(line.c_str() + a + 6);
                        if (b != std::string::npos) r->behind = atoi(line.c_str() + b + 7);
                        std::string br = line.substr(3);
                        size_t dots = br.find("...");
                        if (dots != std::string::npos) br = br.substr(0, dots);
                        size_t sp = br.find(' ');
                        if (sp != std::string::npos) br = br.substr(0, sp);
                        if (!br.empty() && br != "HEAD") r->branch = std::wstring(br.begin(), br.end());
                    } else if (line.rfind("??", 0) == 0) {
                        r->untracked++;
                    } else if (line.size() > 3) {
                        r->changed++;
                    }
                }
            }
        }
        if (!PostMessageW(notify, WM_STRATA_GIT, 0, (LPARAM)r)) delete r;
    }).detach();
}

// ---------------------------------------------------------------- preview handler host
static HWND g_host = nullptr;
static IPreviewHandler* g_handler = nullptr;

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = (HBRUSH)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (b) FillRect((HDC)w, &rc, b);
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

bool PreviewHandlerActive() { return g_handler != nullptr; }

void PreviewHandlerClose() {
    if (g_handler) {
        g_handler->Unload();
        g_handler->Release();
        g_handler = nullptr;
    }
    if (g_host) {
        HBRUSH b = (HBRUSH)GetWindowLongPtrW(g_host, GWLP_USERDATA);
        DestroyWindow(g_host);
        if (b) DeleteObject(b);
        g_host = nullptr;
    }
}

void PreviewHandlerMove(RECT rc) {
    if (!g_host) return;
    SetWindowPos(g_host, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    RECT inner{0, 0, rc.right - rc.left, rc.bottom - rc.top};
    if (g_handler) g_handler->SetRect(&inner);
}

bool PreviewHandlerShow(HWND parent, const std::wstring& path, const std::wstring& clsidStr, RECT rc, COLORREF bg, COLORREF fg) {
    PreviewHandlerClose();
    CLSID clsid;
    if (FAILED(CLSIDFromString(clsidStr.c_str(), &clsid))) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"StrataPreviewHost";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    g_host = CreateWindowExW(0, L"StrataPreviewHost", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, rc.left, rc.top,
                             rc.right - rc.left, rc.bottom - rc.top, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_host) return false;
    SetWindowLongPtrW(g_host, GWLP_USERDATA, (LONG_PTR)CreateSolidBrush(bg));
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_handler));
    if (FAILED(hr) || !g_handler) { Log(L"Preview handler create failed 0x%08lx", hr); PreviewHandlerClose(); return false; }
    bool inited = false;
    IInitializeWithItem* iwi = nullptr;
    IInitializeWithFile* iwf = nullptr;
    IInitializeWithStream* iws = nullptr;
    if (SUCCEEDED(g_handler->QueryInterface(IID_PPV_ARGS(&iwi)))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            inited = SUCCEEDED(iwi->Initialize(item, STGM_READ));
            item->Release();
        }
        iwi->Release();
    }
    if (!inited && SUCCEEDED(g_handler->QueryInterface(IID_PPV_ARGS(&iwf)))) {
        inited = SUCCEEDED(iwf->Initialize(path.c_str(), STGM_READ));
        iwf->Release();
    }
    if (!inited && SUCCEEDED(g_handler->QueryInterface(IID_PPV_ARGS(&iws)))) {
        IStream* st = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &st))) {
            inited = SUCCEEDED(iws->Initialize(st, STGM_READ));
            st->Release();
        }
        iws->Release();
    }
    if (!inited) { PreviewHandlerClose(); return false; }
    IPreviewHandlerVisuals* vis = nullptr;
    if (SUCCEEDED(g_handler->QueryInterface(IID_PPV_ARGS(&vis)))) {
        vis->SetBackgroundColor(bg);
        vis->SetTextColor(fg);
        vis->Release();
    }
    RECT inner{0, 0, rc.right - rc.left, rc.bottom - rc.top};
    g_handler->SetWindow(g_host, &inner);
    hr = g_handler->DoPreview();
    if (FAILED(hr)) { Log(L"DoPreview failed 0x%08lx", hr); PreviewHandlerClose(); return false; }
    return true;
}
