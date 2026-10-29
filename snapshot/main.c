// Snapshots: takes a host-side snapshot of each run every few seconds to rekindle_run_<mgid>.bin,
// and after a restore puts back ground items, runes, the player number, graces, enemy HP and the day.
#include "snapshot.h"

typedef int32_t HSteamUser;
typedef HSteamUser (*GetHSteamUser_t)(void);
typedef void *(*FindOrCreateUserInterface_t)(HSteamUser, const char *);

uint64_t first_snapshot_ms = 10000, snapshot_every_ms = 30000, resume_snapshots_ms = 60000, stop_before_boss_ms = 60000;
bool restore_items = true, restore_runes = true, boss_drops = true, graces = true, enemy_hp = true;

// Checks the run marker once a second: a new run gets its first snapshot first_snapshot_ms after the
// marker is set, retried every 10 s until one is finished.
static DWORD WINAPI marker_thread(LPVOID unused)
{
    (void)unused;
    uint64_t seen_mgid = 0, seen_ms = 0, asked_ms = 0;
    for (;; Sleep(1000)) {
        uint64_t m = *(volatile uint64_t *)&run_mgid, t = unix_ms();
        if (m != seen_mgid)
            seen_mgid = m, seen_ms = t;
        if (m && m != done_mgid && t - seen_ms > first_snapshot_ms && t - asked_ms > 10000)
            asked_ms = t, InterlockedExchange(&run_request, 1);
    }
    return 0;
}

static void load_settings(void)
{
    first_snapshot_ms = cfg("snapshots", "first_after_s", 10) * 1000ull;
    snapshot_every_ms = cfg("snapshots", "every_s", 30) * 1000ull;
    resume_snapshots_ms = cfg("snapshots", "resume_after_restore_s", 60) * 1000ull;
    stop_before_boss_ms = cfg("snapshots", "stop_before_boss_s", 60) * 1000ull;
    restore_items = cfg("restore", "restore_ground_items", 1);
    restore_runes = cfg("restore", "restore_runes", 1);
    boss_drops = cfg("restore", "fix_boss_drops", 1);
    graces = cfg("restore", "fix_graces", 1);
    enemy_hp = cfg("restore", "restore_enemy_hp", 1);
    log_line(LOG_INFO, "settings first=%llu every=%llu resume=%llu boss=%llu items=%d runes=%d drops=%d graces=%d enemy_hp=%d",
             (unsigned long long)first_snapshot_ms, (unsigned long long)snapshot_every_ms,
             (unsigned long long)resume_snapshots_ms, (unsigned long long)stop_before_boss_ms, restore_items, restore_runes, boss_drops, graces, enemy_hp);
}

// Waits until Steam has a user and its networking interface, then patches the game.
static DWORD WINAPI install(LPVOID unused)
{
    (void)unused;
    load_settings();
    for (int tries = 0; tries < 1200; tries++, Sleep(250)) {
        HMODULE api = GetModuleHandleA("steam_api64.dll");
        if (!api)
            continue;
        GetHSteamUser_t get_user = (GetHSteamUser_t)GetProcAddress(api, "SteamAPI_GetHSteamUser");
        FindOrCreateUserInterface_t find = (FindOrCreateUserInterface_t)GetProcAddress(api, "SteamInternal_FindOrCreateUserInterface");
        if (!get_user || !find) {
            log_line(LOG_ERROR, "steam_api64.dll has no SteamAPI_GetHSteamUser or FindOrCreateUserInterface");
            return 1;
        }
        HSteamUser user = get_user();
        if (!user || !find(user, "SteamNetworking006"))
            continue;
        if (snapshot_install())
            CloseHandle(CreateThread(NULL, 0, marker_thread, NULL, 0, NULL));
        return 0;
    }
    log_line(LOG_ERROR, "Steam networking never came up, snapshots are off");
    return 1;
}

void snapshot_start(void)
{
    CloseHandle(CreateThread(NULL, 0, install, NULL, 0, NULL));
}
