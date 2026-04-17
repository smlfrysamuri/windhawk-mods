// ==WindhawkMod==
// @id              better-uwp-clean
// @name            (Better) UWP Clean
// @description     Automatically end UWP processes when no UWP apps are open, except for shell-critical processes.
// @version         0.4
// @author          Smlfrysamuri
// @github          https://github.com/smlfrysamuri
// @include         ApplicationFrameHost.exe
// @include         TextInputHost.exe
// @include         SystemSettingsAdminFlows.exe
// @include         ctfmon.exe
// @compilerOptions -ldwmapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# (Better) UWP Clean

Automatically terminates non-essential UWP host processes
(ApplicationFrameHost, TextInputHost, SystemSettingsAdminFlows, ctfmon)
when **no** UWP app has a visible window open.

Detection uses window enumeration with cloak-awareness rather than a
hardcoded process-name list, so it works correctly with Calculator,
Photos, Clock, Paint, and every other UWP app.

A grace period at startup ensures the host process is not killed before
the launching app has time to create its window.  The monitor also
requires several consecutive idle checks before terminating, avoiding
false positives during brief transitions between UWP apps.

Shell-critical processes like RuntimeBroker.exe are never targeted.
*/
// ==/WindhawkModReadme==

#include <atomic>
#include <thread>
#include <windows.h>
#include <dwmapi.h>

// --- Tuning constants ---
// How long to wait after init before the first check (ms).
// Gives the launching UWP app time to create its window.
static constexpr DWORD STARTUP_GRACE_MS = 10000;

// Interval between checks during monitoring (ms).
static constexpr DWORD POLL_INTERVAL_MS = 5000;

// How many consecutive "no windows" checks before we terminate.
// Prevents killing the host during brief gaps (app switching, etc.).
static constexpr int IDLE_THRESHOLD = 3;

// --- Globals ---
static std::atomic<bool> g_stopThread{false};
static std::thread g_workerThread;

// ---------------------------------------------------------------
// Window detection
// ---------------------------------------------------------------

static bool IsWindowCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                           &cloaked, sizeof(cloaked)))
           && cloaked != 0;
}

struct EnumCtx {
    bool found = false;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lParam);

    wchar_t className[256]{};
    if (!GetClassNameW(hwnd, className, _countof(className)))
        return TRUE;

    if (wcscmp(className, L"ApplicationFrameWindow") != 0)
        return TRUE;

    if (!IsWindowVisible(hwnd) || IsWindowCloaked(hwnd))
        return TRUE;

    if (GetWindowTextLengthW(hwnd) == 0)
        return TRUE;

    ctx->found = true;
    return FALSE;
}

static bool AnyUwpAppWindowOpen() {
    EnumCtx ctx;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// ---------------------------------------------------------------
// Interruptible sleep — returns true if stop was requested
// ---------------------------------------------------------------
static bool SleepInterruptible(DWORD totalMs) {
    constexpr DWORD STEP = 500;
    for (DWORD elapsed = 0;
         elapsed < totalMs && !g_stopThread.load(std::memory_order_relaxed);
         elapsed += STEP) {
        Sleep(STEP);
    }
    return g_stopThread.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------
// Background monitor thread
// ---------------------------------------------------------------
static void MonitorAndTerminate() {
    // Startup grace period — let the UWP app create its window
    if (SleepInterruptible(STARTUP_GRACE_MS))
        return;

    int idleCount = 0;

    while (!g_stopThread.load(std::memory_order_relaxed)) {
        if (AnyUwpAppWindowOpen()) {
            idleCount = 0;  // reset whenever we see a live window
        } else {
            ++idleCount;
            Wh_Log(L"No visible UWP windows (%d/%d)", idleCount, IDLE_THRESHOLD);

            if (idleCount >= IDLE_THRESHOLD) {
                Wh_Log(L"Idle threshold reached — terminating host process");
                ExitProcess(0);
            }
        }

        if (SleepInterruptible(POLL_INTERVAL_MS))
            return;
    }
}

// ---------------------------------------------------------------
// Windhawk lifecycle
// ---------------------------------------------------------------

BOOL Wh_ModInit() {
    Wh_Log(L"better-uwp-clean: Initializing");

    // Never terminate immediately — always defer to the monitor thread
    // so the launching app has time to create its window.
    g_stopThread.store(false, std::memory_order_relaxed);
    g_workerThread = std::thread(MonitorAndTerminate);

    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"better-uwp-clean: Uninitializing");

    g_stopThread.store(true, std::memory_order_relaxed);
    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }
}