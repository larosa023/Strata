// Strata - wiring between the reusable updater module and the app
#include "autoupdate.h"
#include "dialogs.h"
#include "config.h"
#include "queue.h"
#include <ctime>

void (*g_releaseAppForUpdate)() = nullptr;
static HWND g_owner = nullptr;
static DWORD g_updatedFromPid = 0;
static bool g_installPending = false;
static bool g_installPendingUser = false;   // the pending install was asked for by a click
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
    if (g_installPending) { AutoUpdateInstallNow(g_installPendingUser); return; }
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

void AutoUpdateInstallNow(bool userAsked) {
    if (updater::Get().state != updater::State::Ready) return;
    // Never interrupt a file transfer; automatic installs also wait out games and presentations
    // (when you click Update yourself, you're clearly not in a game)
    bool transfer = QueueBusy();
    bool busy = !userAsked && updater::UserIsBusy();
    if (transfer || busy) {
        g_installPending = true;
        g_installPendingUser = g_installPendingUser || userAsked;
        // retry soon: a click waits only for the transfer, so check every 10 seconds
        SetTimer(g_owner, TIMER_UPDATE_CHECK, (userAsked ? 10 : 60) * 1000, nullptr);
        Log(L"Update install deferred (%ls)", transfer ? L"file transfer running" : L"full-screen app in front");
        return;
    }
    g_installPending = false;
    g_installPendingUser = false;
    std::wstring error;
    bool ok = updater::InstallAndRestart([] { if (g_releaseAppForUpdate) g_releaseAppForUpdate(); }, &error);
    if (!ok) {
        Log(L"Update install failed: %ls", error.c_str());
        NoticeDialog(g_owner, L"Strata couldn't install the update", error, true);
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
