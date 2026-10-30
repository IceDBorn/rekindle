// Shared Roundtable: players in one game party see each other in the Roundtable Hold. The party leader opens a
// session (type 13) and publishes its lobby id in the party lobby; the other members join it.
#include "rekindle.h"
#include "game.h"
#include "patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HUB_KEY "rk_hub"
#define PUPPET_ID 10000220
#define SPAR_EXIT_EVENT 1000002221
#define NET_ONLINE_VT 0x2ca4a10  // net session object class that sends its owner's state
#define NET_OFFLINE_VT 0x2ca49c8
#define BEAST_MODULE_VT 0x2c13008 // CSChrMajyuuModule, the Executor beast
#define PLAYER_INS_VT 0x2c307f8
#define PLAYER_GAME_DATA_VT 0x2be6e28

typedef bool (*Create_t)(void *, uint8_t, uint8_t *);
typedef bool (*JoinSess_t)(void *, uint8_t, void *, uint64_t, uint8_t *);
typedef void (*SmFn_t)(void *);
typedef const char *(*GetData_t)(void *, uint64_t, const char *);
typedef bool (*SetData_t)(void *, uint64_t, const char *, const char *);
typedef int (*GetNum_t)(void *, uint64_t);

static bool enabled;
static uint8_t *base;
static void *mm;
static uint64_t party, session_lobby, joined_lobby;
static bool leader, hub_active, own_call, synced, hub_leaving;
static volatile uint8_t hub_closing;
static uint64_t session_at, retry_at, loaded_at;
static void *create_tramp, *join_tramp, *leave_tramp;
static void run_pending(void);
// A game create or join that comes while the hub session is still closing waits here until the state is 0.
static struct {
    int kind; // 1 create, 2 join
    void *sm;
    uint8_t type, flag;
    uint64_t ids[8], vec[2], a4, at;
} pending;


static uint64_t now(void)
{
    return GetTickCount64();
}

static uint8_t *session(void)
{
    return *(uint8_t **)(base + SESSION_MAN_RVA);
}

static int state(void)
{
    uint8_t *sm = session();
    return sm ? *(int *)(sm + 0xc) : 0;
}

// The game mode byte is 1 in the Roundtable and 2 in an expedition.
static bool in_hub(void)
{
    uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA);
    return gdm && gdm[0xdc] == 1 && !run_mgid;
}

static bool hub_on(void)
{
    int st = state();
    return hub_active && (st == 3 || st == 6) && in_hub();
}

static uint8_t *local_player(void)
{
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA);
    return wcm ? *(uint8_t **)(wcm + 0x174e8) : NULL;
}

#define FOR_MEMBERS(sm, m) \
    for (uint8_t *m = *(uint8_t **)((sm) + 0x78), *m##_end = *(uint8_t **)((sm) + 0x80); m && m < m##_end; m += 0xf0)

static uint64_t member_id(uint8_t *m)
{
    return *(uint64_t *)(m + 0x10);
}

static void leave_session(const char *why)
{
    uint8_t *sm = session();
    note(LOG_INFO, "Hub leave session (%s)", why);
    if (leader && party && mm)
        ((SetData_t)mm_orig[20])(mm, party, HUB_KEY, "");
    hub_active = synced = false, hub_leaving = true, hub_closing = 1;
    session_lobby = joined_lobby = 0, retry_at = now() + 5000;
    if (sm && (state() == 3 || state() == 6))
        ((void (*)(void *, uint8_t))leave_tramp)(sm, 4);
}

// Called for every SetLobbyData("inviteParam"): with no session it marks the party lobby the game just created,
// during our own session start it is the session lobby to publish.
void hub_lobby_created(void *s, uint64_t lobby)
{
    if (!enabled)
        return;
    mm = s;
    if (state() == 0 && in_hub() && lobby != party) {
        party = lobby, leader = true;
        note(LOG_INFO, "Hub party 0x%llx (leader)", (unsigned long long)lobby);
    } else if (hub_active && leader && party && !session_lobby) {
        char v[24];
        session_lobby = lobby;
        snprintf(v, sizeof v, "%llx", (unsigned long long)lobby);
        ((SetData_t)mm_orig[20])(s, party, HUB_KEY, v);
        note(LOG_INFO, "Hub session lobby 0x%llx published", (unsigned long long)lobby);
    }
}

void hub_lobby_joined(void *s, uint64_t lobby)
{
    if (!enabled || lobby == fake_lobby || hub_active || state() != 0)
        return;
    mm = s, party = lobby, leader = false;
    note(LOG_INFO, "Hub party 0x%llx (member)", (unsigned long long)lobby);
}

void hub_lobby_left(uint64_t lobby)
{
    if (!enabled || !party || lobby != party)
        return;
    note(LOG_INFO, "Hub party 0x%llx left", (unsigned long long)lobby);
    if (hub_active)
        leave_session("party left");
    party = 0;
}

// Session steps the map load would run; without them the members never exchange 0x0a/0x0b.
static void host_steps(uint8_t *sm)
{
    ((SmFn_t)(base + 0xdf28e0))(sm);
    ((SmFn_t)(base + 0xdf23c0))(sm);
}

static void guest_steps(uint8_t *sm, int stage)
{
    if (stage == 0)
        ((SmFn_t)(base + 0xdf28b0))(sm);
    else if (stage == 1)
        ((SmFn_t)(base + 0xdf28d0))(sm);
    else {
        ((SmFn_t)(base + 0xdf2870))(sm);
        ((SmFn_t)(base + 0xdf20c0))(sm);
        ((SmFn_t)(base + 0xdf23c0))(sm);
        ((SmFn_t)(base + 0xdf28e0))(sm);
    }
}

// FUN_140ddfca0 sends equipment (0x0c) and 0x62 only when the mode byte is 2; set it around the call and clear
// the sent flag (+0xe6).
static void resend(uint8_t *sm)
{
    uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA), *mode = gdm + 0xdc, saved = *mode;
    FOR_MEMBERS(sm, m) {
        if (m[0xe1])
            continue;
        *mode = 2, m[0xe6] = 0;
        ((void (*)(void *, uint64_t *))(base + 0xddfca0))(*(void **)(sm + 0x178), (uint64_t *)(m + 0x10));
        *mode = saved;
    }
    note(LOG_DEBUG, "Hub resent player data");
}

// Reads a pointer at p + off, or NULL when that memory is not readable.
static uint8_t *ptr_at(uint8_t *p, size_t off)
{
    return p && readable(p + off, 8) ? *(uint8_t **)(p + off) : NULL;
}

// A player still being spawned has garbage in these fields, so every step is checked.
static uint8_t *net_object(uint8_t *p)
{
    uint8_t *c = ptr_at(ptr_at(ptr_at(ptr_at(p, 0x5a8), 0x1e8), 0), 0x18);
    return readable(c, 16) ? c : NULL;
}

// A joined guest's player keeps the offline net class with no owner id, so its state never goes out.
static void netfix(uint8_t *sm)
{
    uint8_t *c = net_object(local_player());
    uint64_t self = 0;
    FOR_MEMBERS(sm, m)
        if (m[0xe1])
            self = member_id(m);
    if (c && self && *(uint8_t **)c == base + NET_OFFLINE_VT)
        *(uint64_t *)(c + 8) = self, *(uint8_t **)c = base + NET_ONLINE_VT;
    note(LOG_DEBUG, "Hub netfix c=%p self=%llx", c, (unsigned long long)self);
}

// Player numbers: the hub gives every local player 1 and remote ones -1, and chr handles carry the number, so a
// peer's throw resolves to the receiver's own player. Host is 1, guests follow in steam id order.
static int number_of(uint8_t *sm, uint64_t id)
{
    int n = 2;
    FOR_MEMBERS(sm, m) {
        if (member_id(m) == id && m[0xe0])
            return 1;
        if (!m[0xe0] && member_id(m) < id)
            n++;
    }
    FOR_MEMBERS(sm, m)
        if (member_id(m) == id)
            return n;
    return 0;
}

static void set_numbers(uint8_t *sm)
{
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA);
    int n = wcm ? *(int *)(*(uint8_t **)(wcm + 0x9910) + 0x120) : 0;
    uint8_t *cs = wcm ? *(uint8_t **)(wcm + 0x170b8 + n * 8) : NULL;
    uint8_t **ents = cs ? *(uint8_t ***)(cs + 0x18) : NULL;
    int cnt = cs ? *(int *)(cs + 0x10) : 0;
    uint64_t self = 0, other = 0;
    int members = 0;
    FOR_MEMBERS(sm, m) {
        if (m[0xe1])
            self = member_id(m);
        else
            other = member_id(m);
        members++;
    }
    for (int k = 0; ents && ents[0] && k < cnt && k < 6; k++) {
        uint8_t *p = ents[k * 2];
        if (ptr_at(p, 0) != base + PLAYER_INS_VT)
            continue;
        uint8_t *pg = ptr_at(p, 0x5a0), *c = net_object(p);
        uint64_t id = k == 0 ? self : c && *(uint8_t **)c == base + NET_ONLINE_VT ? *(uint64_t *)(c + 8) : 0;
        // Remote PlayerGameData sit in an array at GameDataMan+0x10, one 0x8a0 entry per session slot.
        uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA), *arr = ptr_at(gdm, 0x10);
        ptrdiff_t off = arr && pg ? pg - arr : -1;
        if (!id && k > 0 && off >= 0 && off % 0x8a0 == 0 && off / 0x8a0 < 6)
            FOR_MEMBERS(sm, m)
                if (!m[0xe1] && *(int *)(m + 0xd0) == off / 0x8a0)
                    id = member_id(m);
        if (!id && k == 1 && cnt == 2 && members == 2)
            id = other;
        int want = id ? number_of(sm, id) : 0;
        // Remote handles on the guest carry chr-set index 0x7f; use the local player's (bits 11..17).
        uint32_t hw = (*(uint32_t *)(ents[0] + 0x7c) & ~0x7ffu) | want;
        note(LOG_DEBUG, "Hub numbers k=%d p=%p pg=%p id=%llx want=%d", k, p, pg, (unsigned long long)id, want);
        if (!want || ptr_at(pg, 0) != base + PLAYER_GAME_DATA_VT ||
            (*(int *)(pg + 8) == want && *(uint32_t *)(p + 0x7c) == hw))
            continue;
        *(int *)(pg + 8) = want, *(uint32_t *)(p + 0x7c) = hw;
        note(LOG_DEBUG, "Hub player %d number %d", k, want);
        // The Executor beast is spawned at an index built from the number; respawn it.
        uint8_t *cm = *(uint8_t **)(p + 0x1b8), *mod = cm ? *(uint8_t **)(cm + 0x190) : NULL;
        if (mod && *(uint8_t **)mod == base + BEAST_MODULE_VT)
            ((void (*)(void *))(base + 0x457880))(mod);
    }
}

// The hub loads solo, so every PC owns the sparring puppet and its map block; hand both to the host so guests
// follow the host's enemy sync.
static void puppet_owner(uint8_t *sm, bool host)
{
    static uint8_t *last;
    int id = PUPPET_ID, err = 0;
    uint8_t *p = ((uint8_t * (*)(int *, uint64_t, int *))(base + 0x6017f0))(&id, 0, &err);
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA), *cm = wcm ? *(uint8_t **)(wcm + 0x17530) : NULL;
    if (!p || !cm)
        return;
    uint64_t h = *(uint64_t *)(p + 0x78), host_id = 0, owner = 0;
    note(LOG_DEBUG, "Hub puppet p=%p h=%llx", p, (unsigned long long)h);
    if (p != last && ((bool (*)(void *))(base + 0x3d3fe0))(p) != host)
        ((void (*)(void *, uint64_t *, uint8_t))(base + 0x4eafe0))(cm, &h, host);
    last = p;
    FOR_MEMBERS(sm, m)
        if (m[0xe0])
            host_id = member_id(m);
    uint32_t blk = (uint32_t)h;
    uint8_t *fos = *(uint8_t **)(sm + 0x30);
    ((void *(*)(void *, uint64_t *, uint32_t *))(base + 0xdf0400))(sm, &owner, &blk);
    if (!host_id || !fos || owner == host_id)
        return;
    if (host)
        ((void (*)(void *, uint64_t *, uint32_t *))(base + 0xdd7ba0))(fos, &host_id, &blk);
    else
        ((void (*)(void *, uint32_t *, uint64_t *))(base + 0xdd5a30))(fos, &blk, &host_id);
    note(LOG_DEBUG, "Hub puppet block %x owner %llx", blk, (unsigned long long)host_id);
}

// Per member join timeline, in ms after the member shows up in the session (manual test timings).
#define T_HOST_STEPS 6000
#define T_GUEST_STEPS 10000
#define T_RESEND 18000
#define T_NETFIX 22000
#define T_SYNC 25000
#define MAX_PEERS 6

static struct peer {
    uint64_t id, seen;
    int done, retries;
} peers[MAX_PEERS];

static int slot_flags(uint8_t *m)
{
    uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA), *f = gdm ? *(uint8_t **)(gdm + 0x18) : NULL;
    int slot = *(int *)(m + 0xd0);
    return f && slot >= 0 && slot < 6 ? f[slot] : -1;
}

static void reset_peers(void)
{
    memset(peers, 0, sizeof peers);
}

static struct peer *peer_for(uint64_t id)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].id == id)
            return &peers[i];
    for (int i = 0; i < MAX_PEERS; i++)
        if (!peers[i].id) {
            peers[i].id = id, peers[i].seen = now();
            return &peers[i];
        }
    return NULL;
}

static void start_session(void)
{
    uint8_t *sm = session(), one = 1;
    own_call = true;
    bool ok;
    if (leader)
        ok = ((Create_t)create_tramp)(sm, 13, &one);
    else {
        const char *v = mm ? ((GetData_t)mm_orig[19])(mm, party, HUB_KEY) : NULL;
        uint64_t id = v && *v ? strtoull(v, NULL, 16) : 0, vec[2] = {(uint64_t)&id, (uint64_t)&id + 8};
        // The host answers a join only once its session is set up, so wait a few seconds after the id appears.
        static uint64_t seen_id, seen_at;
        if (id != seen_id)
            seen_id = id, seen_at = now();
        if (!id || id == joined_lobby || now() < seen_at + 5000) {
            own_call = false;
            return;
        }
        joined_lobby = id;
        ok = ((JoinSess_t)join_tramp)(sm, 13, vec, 0, &one);
        if (!ok)
            joined_lobby = 0;
        note(LOG_INFO, "Hub join 0x%llx ok=%d", (unsigned long long)id, ok);
    }
    own_call = false;
    hub_active = ok, synced = false, session_at = now(), retry_at = now() + 30000;
    reset_peers();
    if (leader)
        note(LOG_INFO, "Hub session create ok=%d", ok);
}

void hub_tick(void)
{
    static int guest_stage = -1, stage1_ticks;
    static uint64_t sync_at;
    if (!enabled || !base || !session())
        return;
    uint8_t *sm = session();
    run_pending();
    int st = state();
    if (st == 0)
        hub_leaving = false;
    hub_closing = hub_active || hub_leaving;
    if (!local_player() || !in_hub()) {
        loaded_at = 0;
        if (hub_active && local_player() && (st == 3 || st == 6))
            leave_session("left the Roundtable");
        if (hub_active && st == 0)
            hub_active = false;
        return;
    }
    if (!loaded_at)
        loaded_at = now();
    if (hub_active && st == 0) {
        note(LOG_INFO, "Hub session ended");
        hub_active = synced = false, session_lobby = 0, joined_lobby = 0, guest_stage = -1;
        retry_at = now() + 10000;
    }
    if (!hub_active) {
        bool ready = party && st == 0 && now() > loaded_at + 5000 && now() > retry_at;
        if (ready && (!leader || ((GetNum_t)mm_orig[17])(mm, party) >= 2))
            start_session();
        return;
    }
    if (st != 3 && st != 6)
        return;
    if (st == 3)
        sm[0x1f] = 0;
    // A character change drops the player from the session; it starts over when it comes back.
    static uint64_t host_lost;
    bool has_host = st == 3;
    for (int i = 0; i < MAX_PEERS; i++) {
        bool found = false;
        FOR_MEMBERS(sm, m)
            if (member_id(m) == peers[i].id)
                found = true;
        if (peers[i].id && !found) {
            note(LOG_INFO, "Hub %llx left the session", (unsigned long long)peers[i].id);
            memset(&peers[i], 0, sizeof peers[i]);
        }
    }
    FOR_MEMBERS(sm, m)
        if (m[0xe0] && !m[0xe1])
            has_host = true;
    if (has_host || now() < session_at + 15000)
        host_lost = 0;
    else if (!host_lost)
        host_lost = now();
    else if (now() > host_lost + 5000) {
        host_lost = 0;
        leave_session("host dropped");
        return;
    }
    static int last_flags[MAX_PEERS] = {-2, -2, -2, -2, -2, -2};
    FOR_MEMBERS(sm, m) {
        int slot = *(int *)(m + 0xd0), f = slot_flags(m);
        if (slot >= 0 && slot < MAX_PEERS && f != last_flags[slot])
            last_flags[slot] = f, note(LOG_DEBUG, "Hub flags slot %d member %llx = %02x pending %d", slot,
                                     (unsigned long long)member_id(m), f, m[0xe2]);
    }
    FOR_MEMBERS(sm, m) {
        struct peer *pr = m[0xe1] ? NULL : peer_for(member_id(m));
        if (!pr)
            continue;
        uint64_t t = now() - pr->seen;
        if (st == 3 && pr->done == 0 && t > T_HOST_STEPS)
            host_steps(sm), pr->done = 1, note(LOG_DEBUG, "Hub host steps for %llx", (unsigned long long)pr->id);
        if (st == 6 && m[0xe0] && pr->done == 0 && t > T_GUEST_STEPS)
            guest_stage = 0, stage1_ticks = 0, pr->done = 1;
        if (pr->done == 1 && t > T_RESEND)
            resend(sm), pr->done = 2;
        if (pr->done == 2 && t > T_NETFIX) {
            if (st == 6)
                netfix(sm);
            pr->done = 3;
        }
        if (pr->done == 3 && t > T_SYNC) {
            pr->done = 4;
            note(LOG_DEBUG, "Hub %llx joined, spawn pending %d flags %02x", (unsigned long long)pr->id, m[0xe2], slot_flags(m));
            if (!synced)
                note(LOG_INFO, "Hub synced with %llx", (unsigned long long)pr->id);
            synced = true;
        }
        // The host spawns on the guest only once all five join flags arrived; a lost answer leaves one out.
        if (st == 6 && m[0xe0] && pr->done == 4 && m[0xe2] && slot_flags(m) != 0x1f && pr->retries < 5 && t > T_SYNC + 5000) {
            note(LOG_INFO, "Hub join flags %02x, repeating join steps", slot_flags(m));
            pr->retries++, pr->done = 0, pr->seen = now() - T_GUEST_STEPS;
        }
    }
    if (guest_stage >= 0) {
        guest_steps(sm, guest_stage);
        if (guest_stage == 2)
            note(LOG_DEBUG, "Hub guest steps done sub=%d", *(int *)(sm + 0x10));
        if (guest_stage != 1 || ++stage1_ticks >= 120)
            guest_stage = guest_stage == 2 ? -1 : guest_stage + 1;
    }
    // A number change while a player is spawning crashes the game, so wait until every member has spawned.
    static uint64_t numbers_at, spawned_at;
    int others = 0;
    bool spawned = true;
    FOR_MEMBERS(sm, m)
        if (!m[0xe1] && (others++, m[0xe2] || slot_flags(m) != 0x1f))
            spawned = false;
    spawned = spawned && others;
    spawned_at = !spawned ? 0 : spawned_at ? spawned_at : now();
    if (spawned_at && now() > spawned_at + 3000 && now() >= numbers_at)
        numbers_at = now() + 1000, set_numbers(sm);
    if (!synced)
        return;
    // A guest in state 6 counts as offline while this byte is set, and the offline tick re-owns every chr.
    uint8_t *gm = *(uint8_t **)(base + GAME_MAN_RVA);
    if (st == 6 && gm && gm[0xb42])
        gm[0xb42] = 0;
    if (now() < sync_at)
        return;
    sync_at = now() + 1000;
    puppet_owner(sm, st == 3);
}

// Event instances a peer's 0x31 fired recently; echoing them back loops the Sparring Grounds toggle.
static void *remote_ins[16];
static uint64_t remote_at[16];
static volatile LONG remote_next;
static void hook_remote_fire(void *ins, void *b, void *c, void *d)
{
    LONG i = InterlockedIncrement(&remote_next) & 15;
    remote_ins[i] = ins, remote_at[i] = now();
    // FUN_1405abe30 is a two-instruction thunk to the instance's vtable +0x20.
    ((void (*)(void *, void *, void *, void *))(*(void ***)ins)[4])(ins, b, c, d);
}

static bool fired_by_peer(void *ins)
{
    for (int i = 0; i < 16; i++)
        if (remote_ins[i] == ins && now() - remote_at[i] < 3000)
            return true;
    return false;
}

static void *evsync_tramp;
// FUN_1405ac0a0 sends an event's state (msg 0x31). Skip echoes, and the Sparring Grounds exit a peer's entry fires.
static bool hook_evsync(void *ins)
{
    if (hub_on() && (fired_by_peer(ins) || *(int *)((uint8_t *)ins + 0x30) == SPAR_EXIT_EVENT))
        return false;
    return ((bool (*)(void *))evsync_tramp)(ins);
}

static void hook_leave(void *sm, uint8_t reason)
{
    // HostInitInfo times out after 30 s (from 0x140dce6cf) because the hub host never sends it.
    if (hub_on() && (uint8_t *)__builtin_return_address(0) == base + 0xdce6cf)
        return;
    ((void (*)(void *, uint8_t))leave_tramp)(sm, reason);
}

// The game opening its own session (an expedition) while ours is open: close ours first.
static void run_pending(void)
{
    if (pending.kind && now() > pending.at + 15000) {
        note(LOG_ERROR, "Hub dropped deferred game call, session still state %d", state());
        pending.kind = 0;
    }
    if (!pending.kind || state() != 0)
        return;
    own_call = true;
    bool ok = pending.kind == 1 ? ((Create_t)create_tramp)(pending.sm, pending.type, &pending.flag)
                                : ((JoinSess_t)join_tramp)(pending.sm, pending.type, pending.vec, pending.a4,
                                                           &pending.flag);
    own_call = false;
    note(LOG_INFO, "Hub ran deferred game %s after %llu ms, ok=%d", pending.kind == 1 ? "create" : "join",
         (unsigned long long)(now() - pending.at), ok);
    pending.kind = 0;
}

// Leaves the hub session for a game create or join; true when the call has to wait for the leave to finish.
static bool defer_for_game(const char *what)
{
    if (own_call || !hub_active)
        return false;
    leave_session(what);
    retry_at = now() + 60000;
    note(LOG_INFO, "Hub state after leave %d", state());
    return state() != 0;
}

static bool hook_create(void *sm, uint8_t type, uint8_t *flag)
{
    if (!defer_for_game("game session create"))
        return ((Create_t)create_tramp)(sm, type, flag);
    pending.kind = 1, pending.sm = sm, pending.type = type, pending.flag = *flag, pending.at = now();
    return true;
}

static bool hook_join(void *sm, uint8_t type, void *ids, uint64_t a4, uint8_t *flag)
{
    if (!defer_for_game("game session join"))
        return ((JoinSess_t)join_tramp)(sm, type, ids, a4, flag);
    uint64_t *v = ids, n = (v[1] - v[0]) / 8;
    n = n > 8 ? 8 : n;
    memcpy(pending.ids, (void *)v[0], n * 8);
    pending.vec[0] = (uint64_t)pending.ids, pending.vec[1] = (uint64_t)(pending.ids + n);
    pending.kind = 2, pending.sm = sm, pending.type = type, pending.a4 = a4, pending.flag = *flag, pending.at = now();
    return true;
}

// The session teardown sets the party's session-lost flag (+0x64), which fails a guest's matchmaking, then clears
// the local ready flag (FUN_140dfa0a0 with 0). The Roundtable has no session without the hub, so both are undone.
static void hook_teardown_ready(void *party_man, uint8_t ready)
{
    if (hub_active || hub_leaving) {
        ((uint8_t *)party_man)[0x64] = 0;
        note(LOG_INFO, "Hub kept ready state through the session close");
        return;
    }
    ((void (*)(void *, uint8_t))(base + 0xdfa0a0))(party_man, ready);
}

// Matchmaking polls the session after a game create (0x140b1abb0, 0x140df8f20): 1 waits, 3 succeeds, else it
// cancels. A create that waits for the hub session to close sees 7 or 0, so the poll reports waiting instead.
static void *hook_create_poll(void *self, void *out)
{
    (void)self;
    int st = state();
    uint32_t code = pending.kind == 1 || st == 1 ? 1 : st == 3 ? 2 : 3;
    ((void (*)(void *, uint32_t, uint32_t))(base + 0x80d210))(out, code, 0);
    return out;
}

// The same poll after a game join (0x140df8c90): 4 waits, 6 succeeds. A join deferred for the hub close waits.
static void *hook_join_poll(void *self, void *out)
{
    (void)self;
    int st = state();
    uint32_t code = pending.kind == 2 || st == 4 ? 1 : st == 6 ? 2 : 3;
    ((void (*)(void *, uint32_t, uint32_t))(base + 0x80d210))(out, code, 0);
    return out;
}

// The party job also needs members in the party member list (0x140df9420), which the hub session's teardown
// empties. The Steam party lobby still counts them.
static bool hook_party_members(void)
{
    if (((bool (*)(void))(base + 0xdf9420))())
        return true;
    return mm && party && ((GetNum_t)mm_orig[17])(mm, party) >= 2;
}

// The party's matchmaking job fails when the session-lost flag (+0x64) is set, which several session-ending paths
// do. Its check at 0x140dfa5f0 jumps here and passes while the hub session is the one closing.
static bool party_check_install(void)
{
    static const uint8_t pro[6] = {0x80, 0x79, 0x64, 0x00, 0x74, 0x26};
    uint8_t *at = base + 0xdfa5f0, *t = alloc_near(base);
    if (!t || !bytes_match(at, pro, 6))
        return false;
    uint64_t flag = (uint64_t)&hub_closing, fail = (uint64_t)(at + 6), pass = (uint64_t)(at + 0x2c);
    static const uint8_t code[] = {
        0x80, 0x79, 0x64, 0x00,                         // cmp byte [rcx+0x64], 0
        0x74, 0x1f,                                     // je pass
        0x50,                                           // push rax
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,             // mov rax, &hub_closing
        0x80, 0x38, 0x00,                               // cmp byte [rax], 0
        0x58,                                           // pop rax
        0x75, 0x0e,                                     // jne pass
        0xff, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // jmp fail
        0xff, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // pass: jmp pass
    };
    memcpy(t, code, sizeof code);
    memcpy(t + 9, &flag, 8), memcpy(t + 29, &fail, 8), memcpy(t + 43, &pass, 8);
    int32_t rel = (int32_t)(t - (at + 5));
    DWORD old;
    VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xe9, memcpy(at + 1, &rel, 4), at[5] = 0x90;
    VirtualProtect(at, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 6);
    return true;
}

// Points both receive-side calls of FUN_1405abe30 at hook_remote_fire through a stub within rel32 range.
static bool redirect_calls(const uint32_t *sites, int n, uint32_t fn, void *hook)
{
    for (int i = 0; i < n; i++) {
        uint8_t *p = base + sites[i];
        if (!readable(p, 5) || p[0] != 0xe8 || p + 5 + *(int32_t *)(p + 1) != base + fn)
            return false;
    }
    uint8_t *stub = alloc_near(base);
    if (!stub)
        return false;
    uint64_t tgt = (uint64_t)hook;
    stub[0] = 0xff, stub[1] = 0x25, memset(stub + 2, 0, 4), memcpy(stub + 6, &tgt, 8);
    for (int i = 0; i < n; i++) {
        uint8_t *p = base + sites[i];
        int32_t rel = (int32_t)(stub - (p + 5));
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p + 1, &rel, 4);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    return true;
}

static void jump_to(uint8_t *fn, void *target)
{
    uint8_t jmp[14] = {0xff, 0x25, 0, 0, 0, 0};
    uint64_t tgt = (uint64_t)target;
    memcpy(jmp + 6, &tgt, 8);
    DWORD old;
    VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old);
    memcpy(fn, jmp, 14);
    VirtualProtect(fn, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 14);
}

// FUN_1405ac0a0 ends its first 19 bytes with a je, so the trampoline rebuilds that branch with absolute jumps.
static bool hook_evsync_install(void)
{
    static const uint8_t pro[15] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0xf6, 0x41,
                                    0x3c, 0x02, 0x48, 0x8b, 0xd9, 0x0f, 0x84};
    uint8_t *fn = base + 0x5ac0a0, *t;
    if (!bytes_match(fn, pro, 15) || !(t = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)))
        return false;
    uint64_t taken = (uint64_t)(fn + 19 + *(int32_t *)(fn + 15)), back = (uint64_t)(fn + 19);
    memcpy(t, pro, 13);
    t[13] = 0x75, t[14] = 0x0e;
    t[15] = 0xff, t[16] = 0x25, memset(t + 17, 0, 4), memcpy(t + 21, &taken, 8);
    t[29] = 0xff, t[30] = 0x25, memset(t + 31, 0, 4), memcpy(t + 35, &back, 8);
    evsync_tramp = t;
    jump_to(fn, (void *)hook_evsync);
    return true;
}

// FUN_140df1180 has a rip-relative load at +10, so its trampoline rebuilds that load with an absolute address.
static bool hook_leave_install(void)
{
    static const uint8_t pro[10] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20};
    uint8_t *fn = base + 0xdf1180, *t;
    if (!readable(fn, 17) || memcmp(fn, pro, 10) || fn[10] != 0x48 || fn[11] != 0x8b || fn[12] != 0x05 ||
        !(t = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)))
        return false;
    uint64_t abs = (uint64_t)(fn + 17 + *(int32_t *)(fn + 13)), back = (uint64_t)(fn + 17);
    memcpy(t, pro, 10);
    t[10] = 0x48, t[11] = 0xb8, memcpy(t + 12, &abs, 8);
    t[20] = 0x48, t[21] = 0x8b, t[22] = 0x00;
    t[23] = 0xff, t[24] = 0x25, memset(t + 25, 0, 4), memcpy(t + 29, &back, 8);
    leave_tramp = t;
    jump_to(fn, (void *)hook_leave);
    return true;
}

DWORD WINAPI hub_thread(LPVOID unused)
{
    (void)unused;
    if (!cfg("hub", "shared_roundtable", 0))
        return 0;
    static const uint8_t create_pro[17] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x88, 0x54, 0x24, 0x10,
                                           0x57, 0x48, 0x81, 0xec, 0x80, 0x06, 0x00, 0x00};
    static const uint8_t join_pro[14] = {0x88, 0x54, 0x24, 0x10, 0x53, 0x56, 0x57,
                                         0x48, 0x81, 0xec, 0x90, 0x06, 0x00, 0x00};
    uint8_t *b = (uint8_t *)GetModuleHandleA(NULL);
    for (int i = 0; i < 1200 && !bytes_match(b + 0xdefbd0, create_pro, 17); i++)
        Sleep(250);
    base = b;
    static const uint8_t poll_pro[9] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x05};
    static const uint32_t fire_sites[2] = {0x5b3033, 0x5b0628}, teardown_site[1] = {0xdf664e},
                          members_site[1] = {0xdfa622};
    if (!hook_leave_install() || !hook_evsync_install() || !redirect_calls(fire_sites, 2, 0x5abe30, hook_remote_fire) ||
        !redirect_calls(teardown_site, 1, 0xdfa0a0, hook_teardown_ready) || !bytes_match(b + 0xb1abb0, poll_pro, 9) ||
        !bytes_match(b + 0xdf8f20, poll_pro, 9) || !bytes_match(b + 0xdf8c90, poll_pro, 9) || !party_check_install() ||
        !redirect_calls(members_site, 1, 0xdf9420, hook_party_members) ||
        !(create_tramp = patch_prologue(0xdefbd0, create_pro, 17, (void *)hook_create, "hub create")) ||
        !(join_tramp = patch_prologue(0xdf0f70, join_pro, 14, (void *)hook_join, "hub join"))) {
        note(LOG_ERROR, "Shared Roundtable hooks failed, feature off");
        return 0;
    }
    jump_to(b + 0xb1abb0, (void *)hook_create_poll);
    jump_to(b + 0xdf8f20, (void *)hook_create_poll);
    jump_to(b + 0xdf8c90, (void *)hook_join_poll);
    enabled = true;
    return 0;
}
