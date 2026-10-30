// Lobby relay for co-op rejoin after everyone crashed. The first player to restore creates a public lobby
// tagged nrk_mgid=<mgid>; another player restoring the same run finds it by lobby search and maps its own id to it.
#include "rekindle.h"
#include <stdio.h>
#include <string.h>

uint64_t game_lobby, real_lobby, restore_mgid;
static uint64_t create_call, search_call, found_lobby;
static bool lobby_decided, found_ready, waiting, world_ready;
static uint64_t wait_since;
void *mm_self, *utils_self;
static void *mm_stub[44];
typedef uint64_t (*Fwd_t)(void *, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*Create_t)(void *, int, int);
typedef void (*StrFilter_t)(void *, const char *, const char *, int);
typedef void (*IntFilter_t)(void *, int);
typedef uint64_t (*Call_t)(void *);
#define RELAY_KEY "nrk_mgid"
#define READY_KEY "nrk_ready"
#define READY_WAIT_MS 180000
typedef const char *(*GetData_t)(void *, uint64_t, const char *);

uint64_t tr(uint64_t v)
{
    return real_lobby && v == game_lobby ? real_lobby : v;
}

// The game's first RequestLobbyData during a restore decides: join a found relay lobby, or create one.
static void relay_decide(uint64_t lobby)
{
    if (lobby_decided || !restore_mgid || !mm_self)
        return;
    // Joining while the first restorer still talks to its fake host breaks both: the fake host has our id.
    if (found_lobby && !found_ready && lobby == fake_lobby) {
        if (!waiting)
            wait_since = unix_ms(), note(LOG_INFO, "Relay 0x%llx not ready, waiting", (unsigned long long)found_lobby);
        waiting = true;
        return;
    }
    lobby_decided = true;
    waiting = false;
    if (found_lobby) {
        game_lobby = lobby, real_lobby = found_lobby;
        fake_lobby = 0, fake_host = 0, run_lobby = found_lobby;
        save_mates();
        note(LOG_INFO, "Relay join 0x%llx as 0x%llx", (unsigned long long)found_lobby, (unsigned long long)lobby);
    } else if (lobby == fake_lobby && lobby_from_save) {
        lobby_decided = false;
    } else if (lobby == fake_lobby) {
        game_lobby = lobby;
        create_call = ((Create_t)mm_orig[13])(mm_self, 2, 3);
        note(LOG_INFO, "Relay create call=0x%llx", (unsigned long long)create_call);
    }
}

// LobbyDataUpdate for the save's lobby: alive means a teammate kept the run, so the game's own rejoin
// takes over and the restore stands down; dead means the restore goes on as usual.
void relay_resolve(bool live)
{
    lobby_from_save = false;
    if (!live) {
        relay_decide(fake_lobby);
        return;
    }
    note(LOG_INFO, "Rejoin lobby 0x%llx is alive, the game rejoins it", (unsigned long long)fake_lobby);
    fake_lobby = 0, fake_host = 0, restore_mgid = 0, lobby_decided = true;
    snapshot_restoring(0, 0);
}

bool relay_hold_505(void)
{
    return waiting;
}

void relay_ready(void)
{
    world_ready = true;
    if (real_lobby && mm_self && !found_lobby) {
        ((Fwd_t)mm_stub[20])(mm_self, real_lobby, (uint64_t)READY_KEY, (uint64_t)"1");
        note(LOG_INFO, "Relay lobby 0x%llx ready", (unsigned long long)real_lobby);
    }
}

static bool call_done(uint64_t call, void *buf, int cub, int id)
{
    bool failed = false;
    if (!((Done_t)utils_orig[11])(utils_self, call, &failed))
        return false;
    memset(buf, 0, cub);
    if (!((Result_t)utils_orig[13])(utils_self, call, buf, cub, id, &failed) || failed)
        memset(buf, 0xff, cub);
    return true;
}

// Runs from RunCallbacks: searches for a relay lobby until the restore decides, and finishes CreateLobby.
void relay_tick(void)
{
    static uint64_t search_at;
    uint64_t now = unix_ms();
    if (!mm_self || !utils_self)
        return;
    if (restore_mgid && !lobby_decided && !search_call && now - search_at > 5000) {
        char v[24];
        snprintf(v, sizeof v, "%llu", (unsigned long long)restore_mgid);
        search_at = now;
        ((StrFilter_t)mm_orig[5])(mm_self, RELAY_KEY, v, 0);
        ((IntFilter_t)mm_orig[9])(mm_self, 3);
        search_call = ((Call_t)mm_orig[4])(mm_self);
    }
    if (waiting && (found_ready || now - wait_since > READY_WAIT_MS)) {
        found_ready = true;
        relay_decide(fake_lobby);
        ((Fwd_t)mm_stub[28])(mm_self, found_lobby, 0, 0);
    }
    if (search_call) {
        // LobbyMatchList_t (510): lobbies matching u32.
        uint32_t r;
        if (call_done(search_call, &r, 4, 510)) {
            search_call = 0;
            if (r != 0xffffffff && r > 0 && !lobby_decided) {
                ((GetId_t)mm_orig[12])(mm_self, &found_lobby, 0);
                const char *v = ((GetData_t)mm_orig[19])(mm_self, found_lobby, READY_KEY);
                found_ready = v && *v == 0x31;
                note(LOG_INFO, "Relay found %u, lobby 0x%llx ready=%d", r, (unsigned long long)found_lobby, found_ready);
            }
        }
    }
    if (create_call) {
        // LobbyCreated_t (513): result u32, lobby u64 at 8.
        uint8_t r[16];
        if (!call_done(create_call, r, 16, 513))
            return;
        create_call = 0;
        uint32_t res;
        memcpy(&res, r, 4);
        if (res != 1) {
            note(LOG_WARN, "Relay create failed result=%u", res);
            return;
        }
        char v[24];
        snprintf(v, sizeof v, "%llu", (unsigned long long)restore_mgid);
        memcpy(&real_lobby, r + 8, 8);
        run_lobby = real_lobby;
        save_mates();
        ((Fwd_t)mm_stub[20])(mm_self, real_lobby, (uint64_t)"inviteParam", (uint64_t)"0");
        ((Fwd_t)mm_stub[20])(mm_self, real_lobby, (uint64_t)RELAY_KEY, (uint64_t)v);
        note(LOG_INFO, "Relay lobby 0x%llx", (unsigned long long)real_lobby);
        if (world_ready)
            relay_ready();
    }
}

// Matchmaking slots without stack arguments: map the game's lobby id to the real one, then log and call.
#define TR_SLOT(n) \
    static uint64_t tr##n(void *s, uint64_t a, uint64_t b, uint64_t c) \
    { \
        return ((Fwd_t)mm_stub[n])(s, tr(a), tr(b), tr(c)); \
    }
TR_SLOT(16) TR_SLOT(19) TR_SLOT(21) TR_SLOT(23) TR_SLOT(24) TR_SLOT(25) TR_SLOT(26)
TR_SLOT(31) TR_SLOT(32) TR_SLOT(33) TR_SLOT(34) TR_SLOT(36) TR_SLOT(37)

static uint64_t tr20(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    if (b && !strcmp((const char *)b, "inviteParam"))
        hub_lobby_created(s, tr(a));
    return ((Fwd_t)mm_stub[20])(s, tr(a), tr(b), tr(c));
}

static uint64_t tr13(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    reset_mates();
    return ((Fwd_t)mm_stub[13])(s, a, b, c);
}

static uint64_t tr15(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    hub_lobby_left(tr(a));
    if (real_lobby && tr(a) == real_lobby)
        note(LOG_INFO, "Relay leave 0x%llx", (unsigned long long)real_lobby);
    return ((Fwd_t)mm_stub[15])(s, tr(a), b, c);
}

static uint64_t tr28(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    mm_self = s;
    relay_decide(a);
    return ((Fwd_t)mm_stub[28])(s, tr(a), b, c);
}

// Matchmaking slots that carry a lobby id go through tr; mm_stub keeps the logging thunks they forward to.
void relay_wrap_mm(void **nv)
{
    memcpy(mm_stub, nv, 38 * sizeof *nv);
    static void *const tr_fns[][2] = {
        {(void *)13, tr13}, {(void *)15, tr15}, {(void *)16, tr16}, {(void *)19, tr19}, {(void *)20, tr20}, {(void *)21, tr21},
        {(void *)23, tr23}, {(void *)24, tr24}, {(void *)25, tr25}, {(void *)26, tr26}, {(void *)28, tr28},
        {(void *)31, tr31}, {(void *)32, tr32}, {(void *)33, tr33}, {(void *)34, tr34}, {(void *)36, tr36},
        {(void *)37, tr37}};
    for (size_t i = 0; i < sizeof tr_fns / sizeof *tr_fns; i++)
        nv[(uintptr_t)tr_fns[i][0]] = tr_fns[i][1];
}
