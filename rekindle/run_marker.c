// Run marker: rekindle_active_run.txt holds the multi_game_id of the run in progress. GetMultiGameID sets it and
// GetResultData (the run ended) clears it. A quit only sends DeleteSession, so the run stays restorable.
#include "rekindle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snapshot_format.h"

// Members of the lobby the game last created or joined; they join before the run has an id, so the list is
// kept from then on. rekindle_mates.txt holds "<mgid> <id>..."; a restore uses the first id as the fake host, so the
// slot the game keeps for it matches the teammate when they rejoin. A second line "lobby <id>" holds the run's
// Steam lobby, so a restore after a quit can rejoin teammates who are still in it.
uint64_t run_mgid, run_mates[3], run_lobby;

// The trio teammate who is not the fake host, or 0 in a duo.
uint64_t trio_mate(void)
{
    for (int i = 0; i < 3; i++)
        if (run_mates[i] && run_mates[i] != fake_host && run_mates[i] != fake_self)
            return run_mates[i];
    return 0;
}

void save_mates(void)
{
    char path[MAX_PATH];
    mod_path(path, MATES_FILE);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%llu", (unsigned long long)run_mgid);
    for (int i = 0; i < 3 && run_mates[i]; i++)
        fprintf(f, " %llu", (unsigned long long)run_mates[i]);
    fprintf(f, "\nlobby %llu\n", (unsigned long long)run_lobby);
    fclose(f);
}

void reset_mates(void)
{
    if (!run_mgid)
        memset(run_mates, 0, sizeof run_mates), run_lobby = 0;
}

void note_mate(uint64_t id, uint64_t lobby)
{
    if (id >> 52 != 0x011 || id == fake_self)
        return;
    if (lobby && lobby != run_lobby && !run_mgid)
        run_lobby = lobby;
    int i = 0;
    while (i < 3 && run_mates[i] && run_mates[i] != id)
        i++;
    if (i == 3 || run_mates[i])
        return;
    run_mates[i] = id;
    if (run_mgid)
        save_mates();
    note(LOG_DEBUG, "Lobby mate %llu", (unsigned long long)id);
}

static void set_marker(const char *path, unsigned long long m)
{
    if (m == run_mgid)
        return;
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%llu\n", m);
        fclose(f);
        run_restored = false;
        keep_online(false);
        run_mgid = m;
        save_mates();
        note(LOG_INFO, "Run marker set mgid=%llu", m);
    }
}

void run_marker_http(const void *data, size_t n)
{
    const uint8_t *s = data, *p;
    char path[MAX_PATH];
    if (!s || !n)
        return;
    mod_path(path, ACTIVE_RUN_FILE);
    // The host gets the id from GetMultiGameID; a guest first sees it in its JoinMultiplay request.
    if ((find_bytes(s, n, "\"ResponseGetMultiGameIDParams\"") ||
         (find_bytes(s, n, "\"RequestJoinMultiplayParams\"") && find_bytes(s, n, "\"relation_type\":\"GUEST\""))) &&
        (p = find_bytes(s, n, "\"multi_game_id\":\""))) {
        unsigned long long m = strtoull((const char *)p + 17, NULL, 10);
        if (m)
            set_marker(path, m);
    } else if (find_bytes(s, n, "\"RequestGetResultDataParams\"")) {
        if (DeleteFileA(path))
            note(LOG_INFO, "Run marker cleared");
        run_mgid = 0, run_lobby = 0;
        memset(run_mates, 0, sizeof run_mates);
        mod_path(path, MATES_FILE);
        DeleteFileA(path);
    } else if (find_bytes(s, n, "\"RequestDeleteSessionParams\"") && run_mgid) {
        // Left the run: keep the files for a restore, but the next run starts its own mate list.
        note(LOG_INFO, "Run %llu left, marker kept", (unsigned long long)run_mgid);
        run_mgid = 0, run_lobby = 0;
        memset(run_mates, 0, sizeof run_mates);
    }
}
