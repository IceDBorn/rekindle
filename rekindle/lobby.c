// Fake lobby: during a restore the game rejoins fake_lobby, and Steam's answers for it are rewritten so the dead
// lobby looks like a live one owned by fake_host.
#include "rekindle.h"
#include <string.h>

uint64_t fake_lobby, fake_host, fake_self;
static uint64_t fake_join_call;
bool run_restored, lobby_from_save;
void **mm_orig, **utils_orig;
uint64_t host_gone_at;
volatile LONG host_gone;

typedef uint64_t *(*GetMember_t)(void *, uint64_t *, uint64_t, int);
typedef int (*GetNum_t)(void *, uint64_t);
typedef uint64_t (*Join_t)(void *, uint64_t);

static uint64_t *fk_owner(void *self, uint64_t *ret, uint64_t lobby)
{
    if (lobby != fake_lobby || (host_gone && real_lobby))
        return ((GetId_t)mm_orig[35])(self, ret, tr(lobby));
    *ret = host_gone ? fake_self : fake_host;
    note(LOG_DEBUG, "GetLobbyOwner -> %llu", (unsigned long long)*ret);
    return ret;
}

static int fk_num(void *self, uint64_t lobby)
{
    if (lobby != fake_lobby || (host_gone && real_lobby))
        return ((GetNum_t)mm_orig[17])(self, tr(lobby));
    note(LOG_DEBUG, "GetNumLobbyMembers -> %d", host_gone ? 1 : 2);
    return host_gone ? 1 : 2;
}

static uint64_t *fk_member(void *self, uint64_t *ret, uint64_t lobby, int i)
{
    if (lobby != fake_lobby || (host_gone && real_lobby)) {
        ((GetMember_t)mm_orig[18])(self, ret, tr(lobby), i);
        note_mate(*ret, tr(lobby));
        return ret;
    }
    *ret = i == 0 ? fake_self : i == 1 && !host_gone ? fake_host : 0;
    note(LOG_DEBUG, "GetLobbyMemberByIndex %d -> %llu", i, (unsigned long long)*ret);
    return ret;
}

static uint64_t fk_join(void *self, uint64_t lobby)
{
    reset_mates();
    uint64_t call = ((Join_t)mm_orig[14])(self, lobby == fake_lobby ? lobby : tr(lobby));
    if (lobby == fake_lobby) {
        fake_join_call = call;
        run_restored = true;
        if (cfg("restore", "keep_online", 1))
            keep_online(true);
        note(LOG_INFO, "JoinLobby call=0x%llx", (unsigned long long)call);
    }
    return call;
}

static bool fk_done(void *self, uint64_t call, bool *failed)
{
    bool done = ((Done_t)utils_orig[11])(self, call, failed);
    if (call && call == fake_join_call) {
        note(LOG_DEBUG, "IsAPICallCompleted join real=%d failed=%d", done, failed ? *failed : -1);
        if (failed)
            *failed = false;
        return true;
    }
    return done;
}

// LobbyEnter_t (504): lobby u64, permissions u32 at 8, locked at 12, response u32 at 16 (1 = success).
static bool fk_result(void *self, uint64_t call, void *buf, int cub, int expected, bool *failed)
{
    bool ok = ((Result_t)utils_orig[13])(self, call, buf, cub, expected, failed);
    for (int o = 0; ok && real_lobby && o + 8 <= cub; o += 8)
        if (*(uint64_t *)((uint8_t *)buf + o) == real_lobby)
            *(uint64_t *)((uint8_t *)buf + o) = game_lobby;
    if (!call || call != fake_join_call || expected != 504 || cub < 20)
        return ok;
    note(LOG_DEBUG, "GetAPICallResult join real=%d response=%u", ok, ok ? *(uint32_t *)((uint8_t *)buf + 16) : 0);
    memset(buf, 0, cub);
    *(uint64_t *)buf = fake_lobby;
    *(uint32_t *)((uint8_t *)buf + 16) = 1;
    if (failed)
        *failed = false;
    return true;
}

// Points the wrapped vtable slots of each interface kind (see wrap_iface) at the fakes.
void install_fakes(int kind, void **vt, void **nv, int slots)
{
    // Ping needs the P2P wrapper even when a relay join or a live rejoin has already dropped the fake lobby.
    if (kind == 6 && slots > 6)
        emu_wrap_net(vt, nv);
    if (!fake_lobby)
        return;
    if (kind == 1 && slots > 37) {
        mm_orig = vt;
        relay_wrap_mm(nv);
        nv[14] = fk_join;
        nv[17] = fk_num;
        nv[18] = fk_member;
        nv[35] = fk_owner;
    } else if (kind == 4 && slots > 13) {
        utils_orig = vt;
        nv[11] = fk_done;
        nv[13] = fk_result;
    } else if (kind == 5 && slots > 15) {
        emu_wrap_user(vt, nv);
    }
}
