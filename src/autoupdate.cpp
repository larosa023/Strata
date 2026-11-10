// Strata - wiring between the reusable updater module and the app
#include "autoupdate.h"
#include "config.h"
#include "queue.h"
#include <ctime>

void (*g_releaseAppForUpdate)() = nullptr;
static HWND g_owner = nullptr;
static DWORD g_updatedFromPid = 0;
static bool g_installPending = false;
static std::wstring g_failedVersion;

static double UnixNow() { return (double)time(nullptr); }

std::wstring AutoUpdateFailedVersion() { return g_failedVersion; }

void AutoUpdateApplySettings() {
    updater::SetBeta(g_config.channel == L"beta");
    if (!g_owner) return;
    KillTimer(g_owner, TIMER_UPDATE_CHECK);
    if (g_config.autoCheck || g_installPending) SetTimer(g_owner, TIMER_UPDATE_CHECK, 30 * 60 * 1000, nullptr);
}

void AutoUpdateInit(HWND owner, const wchar_t* cmdLine) {
    g_owner = owner;
    updater::Settings s;
    s.owner = L"larosa023";
    s.repo = L"Strata";
    s.exeAsset = L"Strata.exe";
    s.sumsAsset = L"SHA256SUMS.txt";
    s.currentVersion = STRATA_VERSION;
    s.appName = L"Strata";
    s.downloadDir = DataDir() + L"\\updates";
    s.beta = g_config.channel == L"beta";
    s.log = [](const std::wstring& m) { Log(L"%ls", m.c_str()); };
    if (updater::StartedAfterRollback(cmdLine, &g_failedVersion)) {
        Log(L"Started after a rollback; version %ls failed to start", g_failedVersion.c_str());
        s.skipVersion = g_failedVersion;
    }
    updater::Configure(s, owner, WM_STRATA_UPDATE);
    if (updater::StartedAfterUpdate(cmdLine, &g_updatedFromPid)) Log(L"Started after updating (previous pid %lu)", g_updatedFromPid);
    else updater::CleanupLeftovers();
    SetTimer(owner, TIMER_HEALTHY, 2500, nullptr);
}

void AutoUpdateOnStarted() {
    if (g_updatedFromPid) {
        updater::ReportHealthy(g_updatedFromPid);
        g_updatedFromPid = 0;
    }
    if (g_config.autoCheck) SetTimer(g_owner, TIMER_UPDATE_CHECK, 15 * 1000, nullptr);   // first check shortly after start
}

void AutoUpdateOnTimer() {
    SetTimer(g_owner, TIMER_UPDATE_CHECK, 30 * 60 * 1000, nullptr);
    if (g_installPending) { AutoUpdateInstallNow(); return; }
    if (!g_config.autoCheck) return;
    if (UnixNow() >= g_config.lastCheck + g_config.checkHours * 3600.0) updater::CheckAsync(false);
}

void AutoUpdateCheckNow() { updater::CheckAsync(true); }
void AutoUpdateDownloadNow() { updater::DownloadAsync(); }

bool AutoUpdateAvailable() {
    auto st = updater::Get().state;
    return st == updater::State::Available || st == updater::State::Downloading || st == updater::State::Verifying ||
           st == updater::State::Ready;
}

void AutoUpdateInstallNow() {
    if (updater::Get().state != updater::State::Ready) return;
    // Never interrupt a file transfer, a game or a presentation
    if (QueueBusy() || updater::UserIsBusy()) {
        g_installPending = true;
        SetTimer(g_owner, TIMER_UPDATE_CHECK, 60 * 1000, nullptr);
        Log(L"Update install deferred (transfer running or user busy)");
        return;
    }
    g_installPending = false;
    std::wstring error;
    bool ok = updater::InstallAndRestart([] { if (g_releaseAppForUpdate) g_releaseAppForUpdate(); }, &error);
    if (!ok) {
        Log(L"Update install failed: %ls", error.c_str());
        MessageBoxW(g_owner, (L"Strata couldn't install the update.\n\n" + error).c_str(), L"Strata", MB_ICONWARNING);
    }
}

void AutoUpdateOnStatus() {
    updater::Status st = updater::Get();
    if (st.state == updater::State::UpToDate || st.state == updater::State::Available) {
        g_config.lastCheck = UnixNow();
        ConfigSave();
    }
    if (st.state == updater::State::Available && g_config.autoInstall) updater::DownloadAsync();
    else if (st.state == updater::State::Ready && g_config.autoInstall) AutoUpdateInstallNow();
}
