// Strata - transfer queue: copy / move with pause, resume, cancel, skip-on-error, verification,
// speed and time remaining. Jobs run one at a time on a background thread.
#pragma once
#include "common.h"

enum class JobKind { Copy, Move };
enum class JobState { Queued, Scanning, Running, Paused, Done, Failed, Cancelled };

struct JobInfo {
    int id = 0;
    JobKind kind = JobKind::Copy;
    JobState state = JobState::Queued;
    std::wstring title;          // "Copying 12 items to Bids"
    std::wstring dest;           // destination folder
    std::wstring current;        // file being copied
    uint64_t totalBytes = 0, doneBytes = 0;
    size_t totalFiles = 0, doneFiles = 0;
    double bytesPerSec = 0;
    double etaSec = -1;
    size_t errors = 0, skipped = 0;
    bool verify = false;
    std::wstring lastError;
};

// One finished job, for refresh + undo
struct JobResult {
    int id = 0;
    JobKind kind = JobKind::Copy;
    bool ok = false, cancelled = false;
    std::wstring dest;
    std::vector<std::pair<std::wstring, std::wstring>> moved;   // (from, to) top-level items that now live at `to`
    std::vector<std::wstring> created;                           // top-level items a copy created
    size_t errors = 0, skipped = 0;
};

// Questions the worker asks the UI (sent synchronously with WM_STRATA_QUEUEASK, lParam = QueueAsk*)
struct QueueAsk {
    enum Kind { Conflict, Error, LowSpace } kind = Conflict;
    std::wstring src, dst, message;
    int answer = 0;              // Conflict: 0 replace, 1 skip, 2 keep both, 3 cancel job
                                 // Error: 0 retry, 1 skip, 2 cancel job;  LowSpace: 0 continue, 1 cancel
    bool applyToAll = false;
};

void QueueInit(HWND notify);
// Copy or move `sources` into `destFolder`. Returns the job id.
int QueueAdd(JobKind kind, const std::vector<std::wstring>& sources, const std::wstring& destFolder, bool verify);
// Move explicit (from, to) pairs (used by undo).
int QueueAddPairs(JobKind kind, const std::vector<std::pair<std::wstring, std::wstring>>& pairs, const std::wstring& title);
void QueuePause(int id, bool pause);
void QueueCancel(int id);
void QueueClearFinished();
std::vector<JobInfo> QueueSnapshot();
bool QueueBusy();                // a job is queued or running
void QueueShutdown();            // cancels everything (app exit)

std::wstring FormatDuration(double seconds);
