// Run marker: rekindle_active_run.txt holds the multi_game_id of the run in progress. GetMultiGameID sets it and
// GetResultData (the run ended) clears it. A quit only sends DeleteSession, so the run stays restorable.
#include "rekindle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "snapshot_format.h"

// Members of the lobby the game last created or joined; they join before the run has an id, so the list is
// kept from then on. rekindle_mates.txt holds "<mgid> <id>..."; a restore uses the first id as the fake host, so the
// slot the game keeps for it matches the teammate when they rejoin. A second line "lobby <id>" holds the run's
// Steam lobby, so a restore after a quit can rejoin teammates who are still in it. A third line "pids <self> <id>..."
// holds each player's server player_id, which the restored player list must carry for a rejoiner to claim its slot.
uint64_t run_mgid, run_mates[3], run_lobby;
uint32_t self_pid, run_pids[3];

uint32_t mate_pid(uint64_t id)
{
    if (id && id == fake_self)
        return self_pid;
    for (int i = 0; i < 3; i++)
        if (id && run_mates[i] == id)
            return run_pids[i];
    return 0;
}

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
    fprintf(f, "\nlobby %llu\npids %u", (unsigned long long)run_lobby, self_pid);
    for (int i = 0; i < 3 && run_mates[i]; i++) {
        uint32_t p = peer_pid(run_mates[i]);
        fprintf(f, " %u", p ? (run_pids[i] = p) : run_pids[i]);
    }
    fputc('\n', f);
    fclose(f);
}

void reset_mates(void)
{
    if (!run_mgid)
        memset(run_mates, 0, sizeof run_mates), memset(run_pids, 0, sizeof run_pids), run_lobby = 0;
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
    run_mates[i] = id, run_pids[i] = 0;
    if (run_mgid)
        save_mates();
    note(LOG_DEBUG, "Lobby mate %llu", (unsigned long long)id);
}

// Steam ids of the other members of the game's session (SessionMan +0x78, stride 0xf0, id +0x10, self +0xe1).
static int session_mates(uint64_t *out, int max)
{
    uint8_t *sm = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + SESSION_MAN_RVA);
    int n = 0;
    for (uint8_t *m = sm ? *(uint8_t **)(sm + 0x78) : NULL, *end = sm ? *(uint8_t **)(sm + 0x80) : NULL;
         m && m < end && n < max; m += 0xf0)
        if (!m[0xe1] && *(uint64_t *)(m + 0x10))
            out[n++] = *(uint64_t *)(m + 0x10);
    return n;
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
        // Lobby history can hold players from the Roundtable who are not in this run; the session cannot.
        uint64_t ids[3];
        int k = session_mates(ids, 3);
        if (k) {
            memset(run_mates, 0, sizeof run_mates), memset(run_pids, 0, sizeof run_pids);
            memcpy(run_mates, ids, k * sizeof *ids);
            note(LOG_INFO, "Run mates from session: %d", k);
        }
        save_mates();
        note(LOG_INFO, "Run marker set mgid=%llu", m);
    }
}

void clear_marker(const char *why)
{
    char path[MAX_PATH];
    mod_path(path, ACTIVE_RUN_FILE);
    if (DeleteFileA(path))
        note(LOG_INFO, "Run marker cleared (%s)", why);
    run_mgid = 0, run_lobby = 0;
    memset(run_mates, 0, sizeof run_mates), memset(run_pids, 0, sizeof run_pids);
    mod_path(path, MATES_FILE);
    DeleteFileA(path);
    hub_marker_cleared();
}

void run_marker_http(const void *data, size_t n)
{
    const uint8_t *s = data, *p;
    char path[MAX_PATH];
    if (!s || !n)
        return;
    mod_path(path, ACTIVE_RUN_FILE);
    if (find_bytes(s, n, "\"ResponseCreateSessionParams\"") && (p = find_bytes(s, n, "\"player_id\":")))
        self_pid = (uint32_t)strtoul((const char *)p + 12, NULL, 10);
    // The host gets the id from GetMultiGameID; a guest first sees it in its JoinMultiplay request.
    if ((find_bytes(s, n, "\"ResponseGetMultiGameIDParams\"") ||
         (find_bytes(s, n, "\"RequestJoinMultiplayParams\"") && find_bytes(s, n, "\"relation_type\":\"GUEST\""))) &&
        (p = find_bytes(s, n, "\"multi_game_id\":\""))) {
        unsigned long long m = strtoull((const char *)p + 17, NULL, 10);
        if (m)
            set_marker(path, m);
    } else if (find_bytes(s, n, "\"RequestGetResultDataParams\"")) {
        clear_marker("result");
    } else if (find_bytes(s, n, "\"RequestDeleteSessionParams\"") && run_mgid) {
        // Left the run: keep the files for a restore, but the next run starts its own mate list.
        note(LOG_INFO, "Run %llu left, marker kept", (unsigned long long)run_mgid);
        run_mgid = 0, run_lobby = 0;
        memset(run_mates, 0, sizeof run_mates), memset(run_pids, 0, sizeof run_pids);
    }
}
