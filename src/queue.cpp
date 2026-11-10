// Strata - transfer queue (see queue.h)
#include "queue.h"
#include "fs.h"
#include <objbase.h>
#include <bcrypt.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <algorithm>
#include <functional>
#include <map>
#include "config.h"

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Job {
    JobInfo info;
    std::vector<std::pair<std::wstring, std::wstring>> pairs;   // top-level (source, full destination)
    std::atomic<bool> cancel{false};
    std::atomic<bool> paused{false};
    HANDLE resume = nullptr;                                     // signaled while not paused
};
}  // namespace

static std::mutex g_lock;
static std::condition_variable g_cv;
static std::deque<std::shared_ptr<Job>> g_jobs;                 // all jobs (finished ones until cleared)
static HWND g_notify = nullptr;
static int g_nextId = 1;
static bool g_started = false;
static std::atomic<bool> g_shutdown{false};
static DWORD g_lastNotify = 0;

std::wstring FormatDuration(double s) {
    if (s < 0) return L"";
    if (s < 60) return std::to_wstring((int)std::max(1.0, s)) + L" sec";
    if (s < 3600) return std::to_wstring((int)(s / 60 + 0.5)) + L" min";
    int h = (int)(s / 3600);
    return std::to_wstring(h) + L" hr " + std::to_wstring((int)((s - h * 3600) / 60)) + L" min";
}

static void Notify(bool force = false) {
    DWORD now = GetTickCount();
    if (!force && now - g_lastNotify < 120) return;
    g_lastNotify = now;
    if (g_notify) PostMessageW(g_notify, WM_STRATA_QUEUE, 0, 0);
}

std::vector<JobInfo> QueueSnapshot() {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<JobInfo> out;
    for (auto& j : g_jobs) out.push_back(j->info);
    return out;
}

bool QueueBusy() {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto& j : g_jobs)
        if (j->info.state == JobState::Queued || j->info.state == JobState::Scanning || j->info.state == JobState::Running ||
            j->info.state == JobState::Paused) return true;
    return false;
}

void QueuePause(int id, bool pause) {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto& j : g_jobs) {
        if (j->info.id != id) continue;
        j->paused = pause;
        if (pause) ResetEvent(j->resume); else SetEvent(j->resume);
        if (j->info.state == JobState::Running || j->info.state == JobState::Paused)
            j->info.state = pause ? JobState::Paused : JobState::Running;
    }
    Notify(true);
}

void QueueCancel(int id) {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto& j : g_jobs) {
        if (j->info.id != id) continue;
        j->cancel = true;
        SetEvent(j->resume);
        if (j->info.state == JobState::Queued) j->info.state = JobState::Cancelled;
    }
    Notify(true);
}

void QueueClearFinished() {
    std::lock_guard<std::mutex> l(g_lock);
    g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [](const std::shared_ptr<Job>& j) {
        return j->info.state == JobState::Done || j->info.state == JobState::Failed || j->info.state == JobState::Cancelled;
    }), g_jobs.end());
    Notify(true);
}

void QueueShutdown() {
    g_shutdown = true;
    std::lock_guard<std::mutex> l(g_lock);
    for (auto& j : g_jobs) { j->cancel = true; SetEvent(j->resume); }
    g_cv.notify_all();
}

// ---------------------------------------------------------------- helpers
static std::wstring VolumeOf(const std::wstring& path) {
    wchar_t vol[MAX_PATH] = {};
    if (!GetVolumePathNameW(path.c_str(), vol, MAX_PATH)) return L"";
    return ToLower(vol);
}

static bool IsInside(const std::wstring& child, const std::wstring& parent) {
    std::wstring c = ToLower(child), p = ToLower(parent);
    return c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == L'\\';
}

static std::wstring KeepBothName(const std::wstring& dst) {
    std::wstring folder = PathParent(dst), leaf = PathLeaf(dst), base = leaf, ext;
    DWORD a = GetFileAttributesW(dst.c_str());
    size_t dot = leaf.find_last_of(L'.');
    if (!(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) && dot != std::wstring::npos && dot > 0) {
        base = leaf.substr(0, dot);
        ext = leaf.substr(dot);
    }
    for (int i = 2; i < 10000; ++i) {
        std::wstring cand = PathJoin(folder, base + L" (" + std::to_wstring(i) + L")" + ext);
        if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) return cand;
    }
    return dst;
}

static std::wstring CopyName(const std::wstring& dst) {
    // duplicating in the same folder: "name - Copy", "name - Copy (2)"
    std::wstring folder = PathParent(dst), leaf = PathLeaf(dst), base = leaf, ext;
    size_t dot = leaf.find_last_of(L'.');
    if (!IsDirectory(dst) && dot != std::wstring::npos && dot > 0) { base = leaf.substr(0, dot); ext = leaf.substr(dot); }
    std::wstring cand = PathJoin(folder, base + L" - Copy" + ext);
    for (int i = 2; GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES && i < 10000; ++i)
        cand = PathJoin(folder, base + L" - Copy (" + std::to_wstring(i) + L")" + ext);
    return cand;
}

static bool Sha256File(const std::wstring& path, std::vector<unsigned char>& out, std::atomic<bool>& cancel) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD read = 0;
        ok = true;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr) && read) {
            if (cancel) { ok = false; break; }
            BCryptHashData(hash, buf.data(), read, 0);
        }
        out.assign(32, 0);
        if (ok) ok = BCryptFinishHash(hash, out.data(), 32, 0) == 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------- the copy engine
namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Entry { std::wstring src, dst; bool dir; uint64_t size; };
}  // namespace

namespace {  // file-local type (keeps it from clashing with same-named types in other files)
struct Runner {
    std::shared_ptr<Job> job;
    JobResult* result;
    int conflictAll = -1;      // remembered "do this for all" conflict answer
    bool skipAllErrors = false;
    uint64_t sampleBytes = 0;
    DWORD sampleTick = 0;
    double rate = 0;

    void Update(const std::function<void(JobInfo&)>& f, bool force = false) {
        { std::lock_guard<std::mutex> l(g_lock); f(job->info); }
        Notify(force);
    }

    int Ask(QueueAsk::Kind kind, const std::wstring& src, const std::wstring& dst, const std::wstring& msg, bool* all) {
        QueueAsk a;
        a.kind = kind;
        a.src = src;
        a.dst = dst;
        a.message = msg;
        DWORD_PTR r = 0;
        if (!SendMessageTimeoutW(g_notify, WM_STRATA_QUEUEASK, 0, (LPARAM)&a, SMTO_NORMAL, 0x7FFFFFFF, &r)) return kind == QueueAsk::Conflict ? 3 : 2;
        if (all) *all = a.applyToAll;
        return a.answer;
    }

    bool WaitIfPaused() {
        if (job->paused) {
            Update([](JobInfo& i) { i.state = JobState::Paused; i.bytesPerSec = 0; }, true);
            WaitForSingleObject(job->resume, INFINITE);
            sampleTick = GetTickCount();
            Update([](JobInfo& i) { if (i.state == JobState::Paused) i.state = JobState::Running; }, true);
        }
        return !job->cancel;
    }

    void Progress(uint64_t done) {
        DWORD now = GetTickCount();
        if (!sampleTick) { sampleTick = now; sampleBytes = done; }
        DWORD dt = now - sampleTick;
        if (dt >= 400) {
            double inst = (double)(done - sampleBytes) * 1000.0 / dt;
            rate = rate <= 0 ? inst : rate * 0.7 + inst * 0.3;   // smoothed
            sampleTick = now;
            sampleBytes = done;
        }
        Update([&](JobInfo& i) {
            i.doneBytes = done;
            i.bytesPerSec = rate;
            i.etaSec = rate > 1 ? (double)(i.totalBytes > done ? i.totalBytes - done : 0) / rate : -1;
        });
    }

    // Error handling: returns true to retry, false to skip; sets cancel on cancel.
    bool OnError(const std::wstring& what, const std::wstring& path, DWORD err) {
        std::wstring msg = what + L"\n" + path + L"\n\n" + ErrorText(err);
        Update([&](JobInfo& i) { i.lastError = msg; });
        if (skipAllErrors || g_skipErrorsDefault) { Update([](JobInfo& i) { i.skipped++; i.errors++; }); return false; }
        bool all = false;
        int a = Ask(QueueAsk::Error, path, L"", msg, &all);
        if (a == 0) return true;
        if (a == 1) { if (all) skipAllErrors = true; Update([](JobInfo& i) { i.skipped++; i.errors++; }); return false; }
        job->cancel = true;
        return false;
    }

    static std::wstring ErrorText(DWORD err) {
        wchar_t* buf = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                       (LPWSTR)&buf, 0, nullptr);
        std::wstring s = buf ? buf : L"Unknown error";
        LocalFree(buf);
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        return s;
    }

    static bool g_skipErrorsDefault;

    void Scan(const std::wstring& src, const std::wstring& dst, std::vector<Entry>& out, uint64_t& bytes) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa)) {
            out.push_back({src, dst, false, 0});   // will report the error when copied
            return;
        }
        if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            uint64_t sz = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            out.push_back({src, dst, false, sz});
            bytes += sz;
            return;
        }
        out.push_back({src, dst, true, 0});
        if (fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return;   // copy junctions as empty folders, never follow them
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(PathJoin(src, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (job->cancel) break;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            Scan(PathJoin(src, fd.cFileName), PathJoin(dst, fd.cFileName), out, bytes);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    static DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                                       HANDLE, HANDLE, LPVOID ctx) {
        auto* c = static_cast<std::pair<Runner*, uint64_t>*>(ctx);
        Runner* r = c->first;
        if (r->job->cancel) return PROGRESS_CANCEL;
        if (r->job->paused) {
            // block inside the copy; CopyFileEx resumes where it was
            r->Update([](JobInfo& i) { i.state = JobState::Paused; i.bytesPerSec = 0; }, true);
            WaitForSingleObject(r->job->resume, INFINITE);
            r->sampleTick = GetTickCount();
            r->Update([](JobInfo& i) { if (i.state == JobState::Paused) i.state = JobState::Running; }, true);
            if (r->job->cancel) return PROGRESS_CANCEL;
        }
        r->Progress(c->second + (uint64_t)transferred.QuadPart);
        return PROGRESS_CONTINUE;
    }

    // Copies one file; returns true when the file is at `dst` (or was deliberately skipped -> false)
    bool CopyOne(const Entry& e, std::wstring& dstOut, uint64_t baseBytes) {
        std::wstring dst = e.dst;
        if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (_wcsicmp(dst.c_str(), e.src.c_str()) == 0) {
                dst = CopyName(dst);
            } else {
                int a = conflictAll;
                if (a < 0) {
                    bool all = false;
                    a = Ask(QueueAsk::Conflict, e.src, dst, L"", &all);
                    if (all) conflictAll = a;
                }
                if (a == 1) { Update([](JobInfo& i) { i.skipped++; }); return false; }
                if (a == 3) { job->cancel = true; return false; }
                if (a == 2) dst = KeepBothName(dst);
                if (a == 0) SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
            }
        }
        Update([&](JobInfo& i) { i.current = PathLeaf(e.src); });
        std::pair<Runner*, uint64_t> ctx{this, baseBytes};
        DWORD flags = COPY_FILE_ALLOW_DECRYPTED_DESTINATION;
        if (e.size > 256ull * 1024 * 1024) flags |= COPY_FILE_NO_BUFFERING;   // big files copy faster unbuffered
        for (;;) {
            BOOL cancelFlag = FALSE;
            if (CopyFileExW(e.src.c_str(), dst.c_str(), CopyProgress, &ctx, &cancelFlag, flags)) break;
            DWORD err = GetLastError();
            if (err == ERROR_REQUEST_ABORTED || job->cancel) { DeleteFileW(dst.c_str()); job->cancel = true; return false; }
            if (!OnError(L"Couldn't copy", e.src, err)) return false;
        }
        if (job->info.verify) {
            Update([&](JobInfo& i) { i.current = L"Verifying " + PathLeaf(e.src); });
            std::vector<unsigned char> a, b;
            if (!Sha256File(e.src, a, job->cancel) || !Sha256File(dst, b, job->cancel) || a != b) {
                if (job->cancel) return false;
                DeleteFileW(dst.c_str());
                OnError(L"The copy didn't match the original (verification failed)", e.src, ERROR_CRC);
                return false;
            }
        }
        dstOut = dst;
        return true;
    }

    void Run() {
        JobInfo& info = job->info;
        bool isMove = info.kind == JobKind::Move;
        Update([](JobInfo& i) { i.state = JobState::Scanning; }, true);

        // Fast path: moving within one drive is just a rename
        std::vector<std::pair<std::wstring, std::wstring>> slow;
        for (auto& p : job->pairs) {
            if (job->cancel) break;
            if (IsInside(p.second, p.first) || _wcsicmp(p.first.c_str(), p.second.c_str()) == 0) {
                if (isMove && _wcsicmp(p.first.c_str(), p.second.c_str()) == 0) continue;   // already there
                if (IsInside(p.second, p.first)) {
                    OnError(L"A folder can't be copied or moved into itself", p.first, ERROR_INVALID_PARAMETER);
                    continue;
                }
            }
            if (isMove && VolumeOf(p.first) == VolumeOf(PathParent(p.second)) && !VolumeOf(p.first).empty()) {
                std::wstring dst = p.second;
                if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    if (IsDirectory(dst) && IsDirectory(p.first)) { slow.push_back(p); continue; }   // merge folders the slow way
                    int a = conflictAll;
                    if (a < 0) { bool all = false; a = Ask(QueueAsk::Conflict, p.first, dst, L"", &all); if (all) conflictAll = a; }
                    if (a == 1) { Update([](JobInfo& i) { i.skipped++; }); continue; }
                    if (a == 3) { job->cancel = true; break; }
                    if (a == 2) dst = KeepBothName(dst);
                }
                for (;;) {
                    if (MoveFileExW(p.first.c_str(), dst.c_str(), a_replace(dst, p.first) ? MOVEFILE_REPLACE_EXISTING : 0)) {
                        result->moved.push_back({p.first, dst});
                        TagsMovedHook(p.first, dst);
                        break;
                    }
                    if (!OnError(L"Couldn't move", p.first, GetLastError())) break;
                }
                continue;
            }
            slow.push_back(p);
        }

        // Plan the rest
        std::vector<Entry> plan;
        uint64_t total = 0;
        for (auto& p : slow) {
            if (job->cancel) break;
            std::wstring dst = p.second;
            if (!isMove && _wcsicmp(p.first.c_str(), dst.c_str()) == 0) dst = CopyName(dst);   // duplicate in place
            Scan(p.first, dst, plan, total);
        }
        size_t files = 0;
        for (auto& e : plan) if (!e.dir) ++files;
        Update([&](JobInfo& i) { i.totalBytes = total; i.totalFiles = files; i.state = JobState::Running; }, true);

        // Enough space?
        if (!plan.empty() && !job->cancel) {
            ULARGE_INTEGER freeBytes{};
            if (GetDiskFreeSpaceExW(info.dest.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < total) {
                std::wstring m = L"The destination has " + FormatSize(freeBytes.QuadPart) + L" free, but " + FormatSize(total) +
                                 L" is needed.";
                if (Ask(QueueAsk::LowSpace, L"", info.dest, m, nullptr) != 0) job->cancel = true;
            }
        }

        // Copy
        uint64_t done = 0;
        std::vector<std::wstring> createdDirs;
        std::vector<std::wstring> copiedFiles;   // sources successfully copied (for move)
        std::map<std::wstring, std::wstring> topRenamed;
        for (size_t k = 0; k < plan.size() && !job->cancel; ++k) {
            Entry e = plan[k];
            if (!WaitIfPaused()) break;
            if (e.dir) {
                for (;;) {
                    if (CreateDirectoryW(e.dst.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) break;
                    if (!OnError(L"Couldn't create the folder", e.dst, GetLastError())) break;
                }
                createdDirs.push_back(e.src);
                continue;
            }
            std::wstring dstOut;
            bool ok = CopyOne(e, dstOut, done);
            done += e.size;
            Progress(done);
            if (ok) {
                copiedFiles.push_back(e.src);
                if (isMove) {
                    SetFileAttributesW(e.src.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (!DeleteFileW(e.src.c_str())) Log(L"Move: couldn't delete source %ls (%lu)", e.src.c_str(), GetLastError());
                }
                if (dstOut != e.dst) topRenamed[e.dst] = dstOut;
            }
            Update([](JobInfo& i) { i.doneFiles++; });
        }
        // Moving folders across drives: remove the (now empty) source folders, deepest first
        if (isMove && !job->cancel)
            for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it) RemoveDirectoryW(it->c_str());

        // Record top-level results for undo / selection
        for (auto& p : slow) {
            std::wstring dst = p.second;
            if (!isMove && _wcsicmp(p.first.c_str(), dst.c_str()) == 0) continue;
            auto r = topRenamed.find(dst);
            if (r != topRenamed.end()) dst = r->second;
            if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (isMove) { result->moved.push_back({p.first, dst}); TagsMovedHook(p.first, dst); }
            else result->created.push_back(dst);
        }
        Update([&](JobInfo& i) {
            i.state = job->cancel ? JobState::Cancelled : (i.errors && i.doneFiles == 0 && i.totalFiles) ? JobState::Failed : JobState::Done;
            i.current.clear();
            i.bytesPerSec = 0;
            i.etaSec = -1;
            i.doneBytes = i.totalBytes;
        }, true);
    }

    static bool a_replace(const std::wstring& dst, const std::wstring& src) {
        return GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES && _wcsicmp(dst.c_str(), src.c_str()) != 0;
    }
    static void TagsMovedHook(const std::wstring& from, const std::wstring& to);
};
}  // namespace

bool Runner::g_skipErrorsDefault = false;

void Runner::TagsMovedHook(const std::wstring&, const std::wstring&) {}   // tags are updated on the UI thread from JobResult

static void Worker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> l(g_lock);
            g_cv.wait(l, [] {
                if (g_shutdown) return true;
                for (auto& j : g_jobs) if (j->info.state == JobState::Queued) return true;
                return false;
            });
            if (g_shutdown) break;
            for (auto& j : g_jobs) if (j->info.state == JobState::Queued) { job = j; break; }
        }
        if (!job) continue;
        auto* result = new JobResult;
        result->id = job->info.id;
        result->kind = job->info.kind;
        result->dest = job->info.dest;
        Runner::g_skipErrorsDefault = g_config.skipErrors;
        Runner r{job, result};
        // Settings > File operations > "When a file already exists"
        if (g_config.conflictDefault == L"replace") r.conflictAll = 0;
        else if (g_config.conflictDefault == L"skip") r.conflictAll = 1;
        else if (g_config.conflictDefault == L"keepboth") r.conflictAll = 2;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        r.Run();
        result->cancelled = job->cancel;
        result->ok = !job->cancel && job->info.errors == 0;
        result->errors = job->info.errors;
        result->skipped = job->info.skipped;
        Log(L"Job %d (%ls) finished: ok=%d cancelled=%d errors=%u", job->info.id, job->info.title.c_str(), result->ok,
            result->cancelled, (unsigned)result->errors);
        if (!PostMessageW(g_notify, WM_STRATA_JOBDONE, 0, (LPARAM)result)) delete result;
    }
    CoUninitialize();
}

void QueueInit(HWND notify) {
    g_notify = notify;
    if (g_started) return;
    g_started = true;
    std::thread(Worker).detach();
}

static int AddJob(JobKind kind, std::vector<std::pair<std::wstring, std::wstring>> pairs, const std::wstring& dest,
                  const std::wstring& title, bool verify) {
    auto job = std::make_shared<Job>();
    job->resume = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    job->pairs = std::move(pairs);
    std::lock_guard<std::mutex> l(g_lock);
    job->info.id = g_nextId++;
    job->info.kind = kind;
    job->info.dest = dest;
    job->info.title = title;
    job->info.verify = verify;
    g_jobs.push_back(job);
    if (g_jobs.size() > 40) {
        auto it = std::find_if(g_jobs.begin(), g_jobs.end(), [](const std::shared_ptr<Job>& j) {
            return j->info.state == JobState::Done || j->info.state == JobState::Cancelled || j->info.state == JobState::Failed;
        });
        if (it != g_jobs.end()) g_jobs.erase(it);
    }
    g_cv.notify_all();
    Notify(true);
    return job->info.id;
}

int QueueAdd(JobKind kind, const std::vector<std::wstring>& sources, const std::wstring& destFolder, bool verify) {
    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    for (auto& s : sources) pairs.push_back({s, PathJoin(destFolder, PathLeaf(s))});
    std::wstring what = sources.size() == 1 ? L"\"" + PathLeaf(sources[0]) + L"\"" : std::to_wstring(sources.size()) + L" items";
    std::wstring title = (kind == JobKind::Copy ? L"Copying " : L"Moving ") + what + L" to " + PathLeaf(destFolder);
    return AddJob(kind, std::move(pairs), destFolder, title, verify);
}

int QueueAddPairs(JobKind kind, const std::vector<std::pair<std::wstring, std::wstring>>& pairs, const std::wstring& title) {
    return AddJob(kind, pairs, pairs.empty() ? L"" : PathParent(pairs[0].second), title, false);
}
