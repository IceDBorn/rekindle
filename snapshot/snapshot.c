// Snapshot: the game's CHR builder syncs every live networked character id to a fake peer,
// and what the game sends that peer is captured. Runs from the sync manager update on the game thread.
#include "snapshot.h"
#include "patch.h"
#include <stdio.h>
#include <string.h>

#define SEND_HELPER_RVA 0xdf2380
#define CHR_BUILDER_RVA 0xde08c0
typedef void (*SendHelper_t)(void *, uint64_t *, uint32_t, const uint8_t *, int);
typedef int (*ChrBuilder_t)(void *, uint64_t *, uint32_t *);
static SendHelper_t helper_trampoline;
static uint8_t fake_peer_obj[0x1000];
uint64_t fake_peer = (uint64_t)(uintptr_t)fake_peer_obj;
static uint32_t snap_ids[512];
static int snap_id_count, snap_stream_count;
static bool snap_guest;
static uint64_t snap_start_ms;
static HANDLE snap_file = INVALID_HANDLE_VALUE;
// Rekindle's own records also go to rekindle_run_<mgid>.state at once, so a quit while streaming keeps them.
static HANDLE state_file = INVALID_HANDLE_VALUE;
static __thread bool in_snapshot;
// Auto snapshots go to a temp file and are renamed to rekindle_run_<multi_game_id>.bin when complete.
uint64_t snap_mgid, done_mgid;
static uint64_t last_auto_ms;
volatile LONG run_request;

// Snapshot record: u32 ms since trigger, u8 type, u32 len, bytes.
void snap_write(uint32_t type, const uint8_t *data, int len)
{
    if (snap_file == INVALID_HANDLE_VALUE || !data || len <= 0)
        return;
    uint8_t hdr[9];
    uint32_t rel = (uint32_t)(unix_ms() - snap_start_ms), l = (uint32_t)len;
    memcpy(hdr, &rel, 4);
    hdr[4] = (uint8_t)type;
    memcpy(hdr + 5, &l, 4);
    DWORD w;
    EnterCriticalSection(&log_lock);
    snap_stream_count += type == 0x37 || type == 0x5e;
    WriteFile(snap_file, hdr, sizeof hdr, &w, NULL);
    WriteFile(snap_file, data, l, &w, NULL);
    if (state_file != INVALID_HANDLE_VALUE && type >= SNAP_MAP_ITEM && type <= SNAP_NUMBERS)
        WriteFile(state_file, hdr, sizeof hdr, &w, NULL), WriteFile(state_file, data, l, &w, NULL);
    LeaveCriticalSection(&log_lock);
}

// Marker record (type 0, 5 bytes: kind, id) naming the request the following records answer.
// Kind 0 is the guest's 0x42 kind 0, 0x10 and up are 0x51 kinds + 0x10, 0xff ends the build.
void snap_mark(uint8_t kind, uint32_t id)
{
    uint8_t rec[5] = {kind};
    memcpy(rec + 1, &id, 4);
    snap_write(0, rec, sizeof rec);
}

// PROBE: logs game sends to real peers, each type up to 6 times per peer pointer.
static void probe_send(const char *how, uint64_t peer, uint32_t type, const uint8_t *data, int len)
{
    static uint64_t peers[8];
    static uint8_t seen[8][256];
    if (log_level < LOG_DEBUG || !data)
        return;
    int i = 0;
    while (i < 8 && peers[i] && peers[i] != peer)
        i++;
    if (i == 8)
        return;
    peers[i] = peer;
    if (seen[i][type & 0xff]++ >= 6)
        return;
    char hex[3 * 112 + 1] = "";
    for (int k = 0; k < len && k < 112; k++)
        snprintf(hex + 3 * k, 4, "%02x ", data[k]);
    log_line(LOG_DEBUG, "Probe %s peer=%llx type=%02x len=%d %s", how, (unsigned long long)peer, type, len, hex);
}

static void hook_send_helper(void *mgr, uint64_t *peer, uint32_t type, const uint8_t *data, int len)
{
    if (peer && *peer == fake_peer) {
        if (type == 0x1e && data && len >= 0x58)
            memcpy(&snap_mgid, data + 0x50, 8);
        snap_write(type, data, len);
        return;
    }
    probe_send("send", peer ? *peer : 0, type, data, len);
    // A rejoin request (0x60, built at 0x140de15b0: player_id, PlayerGameData +0xec, ...) must carry the run's number;
    // the host's check (FUN_140b4f560) matches it against the restored player list.
    int no = restoring_mgid && type == 0x60 && data && len >= 8 && len <= 256 ? own_recorded_number() : 0;
    if (no && no != *(int *)(data + 4)) {
        uint8_t req[256], *gdm = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + GAME_DATA_MAN_RVA);
        uint8_t *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
        if (pgd)
            *(int *)(pgd + 0xec) = no;
        memcpy(req, data, len), memcpy(req + 4, &no, 4);
        log_line(LOG_INFO, "join-request number %d -> %d", *(int *)(data + 4), no);
        helper_trampoline(mgr, peer, type, req, len);
        return;
    }
    helper_trampoline(mgr, peer, type, data, len);
}

// Kinds 5 and 7 broadcast with FUN_140df24d0(mgr, type, data, len); capture them while take_join runs.
#define BROADCAST_RVA 0xdf24d0
typedef void (*Broadcast_t)(void *, uint32_t, const uint8_t *, int);
static Broadcast_t broadcast_trampoline;
__thread bool in_join_build;

static void hook_broadcast(void *mgr, uint32_t type, const uint8_t *data, int len)
{
    if (in_join_build) {
        snap_write(type, data, len);
        return;
    }
    probe_send("bcast", 0, type, data, len);
    broadcast_trampoline(mgr, type, data, len);
}

// The player list FUN_140b5d2e0 sends as 0x61 (0x20-byte entries at [[this+0xa8]+0x70]: id, slot, player_id, number,
// flags at +0x18, host at +0x19). A restored host got it from the fake host, which stays marked host after leaving.
#define PLAYER_LIST_RVA 0xb5d2e0
typedef void (*PlayerList_t)(uint8_t *, void *);
static PlayerList_t player_list_trampoline;

static void hook_player_list(uint8_t *self, void *peer)
{
    uint8_t *ses = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + SESSION_MAN_RVA), *l = *(uint8_t **)(self + 0xa8);
    uint64_t me = 0;
    for (uint8_t *m = ses ? *(uint8_t **)(ses + 0x78) : NULL, *end = ses ? *(uint8_t **)(ses + 0x80) : NULL; m && m < end;
         m += 0xf0)
        if (m[0xe1])
            me = *(uint64_t *)(m + 0x10);
    // A joiner's request (FUN_140b4f560) must match its id, player_id and number from the run. The list came
    // from the fake host, so a snapshot taken as guest can hold wrong numbers; the recorded ones replace them.
    if (me && *(int *)(ses + 0xc) == 3) {
        for (uint8_t *e = *(uint8_t **)(l + 0x70); e < *(uint8_t **)(l + 0x78); e += 0x20) {
            bool host = *(uint64_t *)e == me;
            int no = run_number_for(*(uint64_t *)e, *(int *)(e + 0x10));
            if (no != *(int *)(e + 0x10)) {
                log_line(LOG_INFO, "player-list number %llx %d -> %d", *(unsigned long long *)e, *(int *)(e + 0x10), no);
                *(int *)(e + 0x10) = no;
            }
            if (e[0x19] != host)
                log_line(LOG_INFO, "player-list host %llx -> %d (pid=%d no=%d)", *(unsigned long long *)e, host,
                         *(int *)(e + 0xc), *(int *)(e + 0x10));
            e[0x19] = host;
        }
    }
    player_list_trampoline(self, peer);
}

// FUN_140df13d0 fills the id-to-number map at [SessionMan+0x168] (16-byte {id, no} entries, count at +0x38) from
// the received player list.
#define FILL_NUMBERS_RVA 0xdf13d0
typedef void (*FillNumbers_t)(uint8_t *, void *);
static FillNumbers_t fill_numbers_trampoline;

// The joiner's session members get numbers of their own (fake host 1, self 2) while the table already holds the
// run's; during a restore the members take the table's numbers before the world loads.
static void hook_fill_numbers(uint8_t *ses, void *vec)
{
    fill_numbers_trampoline(ses, vec);
    uint8_t *t = *(uint8_t **)(ses + 0x168), *gdm = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + GAME_DATA_MAN_RVA);
    uint8_t *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    for (uint8_t *m = restoring_mgid && t ? *(uint8_t **)(ses + 0x78) : NULL; m && m < *(uint8_t **)(ses + 0x80); m += 0xf0)
        for (uint64_t i = 0; i < *(uint64_t *)(t + 0x38) && i < 3; i++)
            if (*(uint64_t *)(t + 16 * i) == *(uint64_t *)(m + 0x10)) {
                *(int *)(m + 0xd4) = *(int *)(t + 16 * i + 8);
                if (m[0xe1] && pgd)
                    *(int *)(pgd + 8) = *(int *)(pgd + 0xec) = *(int *)(m + 0xd4);
            }
}

static void patch_send_helper(void)
{
    // mov rax,[rcx+0x80]; mov r10,rdx; mov rdx,[rcx+0x78]
    static const uint8_t helper[14] = {0x48, 0x8b, 0x81, 0x80, 0, 0, 0, 0x4c, 0x8b, 0xd2, 0x48, 0x8b, 0x51, 0x78};
    // three register spills to the home area, then push rdi
    static const uint8_t bcast[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10,
                                      0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
    helper_trampoline = (SendHelper_t)patch_prologue(SEND_HELPER_RVA, helper, 14, (void *)hook_send_helper,
                                                     "send helper prologue");
    broadcast_trampoline = (Broadcast_t)patch_prologue(BROADCAST_RVA, bcast, 16, (void *)hook_broadcast,
                                                       "broadcast prologue");
    // mov [rsp+8],rbx; push rsi; push rdi; push r14; sub rsp,0x30; mov r14,rdx
    static const uint8_t plist[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x56, 0x57, 0x41,
                                      0x56, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b, 0xf2};
    // mov [rsp+0x10],rdx; push rbx; push rbp; push rsi; push rdi; push r14; sub rsp,0x20
    static const uint8_t fill[15] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20};
    fill_numbers_trampoline = (FillNumbers_t)patch_prologue(FILL_NUMBERS_RVA, fill, 15, (void *)hook_fill_numbers,
                                                            "number map prologue");
    player_list_trampoline = (PlayerList_t)patch_prologue(PLAYER_LIST_RVA, plist, 16, (void *)hook_player_list,
                                                          "player list prologue");
}

// Walks the MSVC std::set<u32> at [[session+0x30]+0x20] that FUN_140df0af0 checks ids against.
static void collect_live_ids(void)
{
    uint8_t *session = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + SESSION_MAN_RVA);
    snap_id_count = 0;
    if (!session || !*(uint8_t **)(session + 0x30))
        return;
    uint8_t *head = *(uint8_t **)(*(uint8_t **)(session + 0x30) + 0x20);
    uint8_t *stack[64], *n = *(uint8_t **)(head + 8);
    int sp = 0;
    while ((n && !n[0x19]) || sp) {
        while (n && !n[0x19] && sp < 64) {
            stack[sp++] = n;
            n = *(uint8_t **)n;
        }
        n = stack[--sp];
        if (snap_id_count < 512)
            snap_ids[snap_id_count++] = *(uint32_t *)(n + 0x20);
        n = *(uint8_t **)(n + 0x10);
    }
}

static void add_world_id(uint32_t id, const uint8_t *chr, int *flagged)
{
    if (snap_id_count < 512)
        snap_ids[snap_id_count++] = id;
    if (chr && chr[0x100])
        (*flagged)++;
}

// Solo has no live id set, so take ids from WorldChrMan's ChrSets (+0x9948, count +0x9a38).
// Array sets keep 0x128-byte entries (id at +0x10); map sets keep a std::map<u32, chr*> at +0x20.
#define CHRSET_ARRAY_VT 0x2c1beb8
#define CHRSET_MAP_VT 0x2c1c0f0
static void collect_world_ids(void)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA);
    snap_id_count = 0;
    if (!wcm)
        return;
    int sets = *(int *)(wcm + 0x9a38);
    for (int i = 0; i < sets && i < 64; i++) {
        uint8_t *set = *(uint8_t **)(wcm + 0x9948 + 8 * i);
        if (!set)
            continue;
        uintptr_t vt = *(uintptr_t *)set - base;
        uint8_t *info = *(uint8_t **)(set + 8);
        int before = snap_id_count, flagged = 0;
        if (vt == CHRSET_ARRAY_VT) {
            uint8_t *e = *(uint8_t **)(set + 0x20);
            int n = *(int *)(set + 0x18);
            for (int k = 0; e && k < n; k++)
                add_world_id(*(uint32_t *)(e + k * 0x128 + 0x10), e + k * 0x128, &flagged);
        } else if (vt == CHRSET_MAP_VT) {
            uint8_t *head = *(uint8_t **)(set + 0x20);
            uint8_t *stack[64], *n = *(uint8_t **)(head + 8);
            int sp = 0;
            while ((n && !n[0x19]) || sp) {
                while (n && !n[0x19] && sp < 64) {
                    stack[sp++] = n;
                    n = *(uint8_t **)n;
                }
                n = stack[--sp];
                add_world_id(*(uint32_t *)(n + 0x20), *(uint8_t **)(n + 0x28), &flagged);
                n = *(uint8_t **)(n + 0x10);
            }
        }
        log_line(LOG_DEBUG, "chrset %d id=%d vt=0x%llx ids=%d flagged=%d", i, info ? *(int *)(info + 0xc) : -1,
                 (unsigned long long)vt, snap_id_count - before, flagged);
    }
}

// Runs on the game thread before the sync manager update, which then streams the queued 0x37 records.
static void take_snapshot(void)
{
    uint64_t now = unix_ms();
    uint8_t *ses = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + SESSION_MAN_RVA);
    bool guest = ses && *(int *)(ses + 0xc) == 6;
    // A guest's live id set holds only the few characters it syncs; dead enemies need their 0x43 too.
    if (!guest)
        collect_live_ids();
    if (guest || !snap_id_count)
        collect_world_ids();
    log_line(LOG_DEBUG, "snapshot-start ms=%llu ids=%d", (unsigned long long)now, snap_id_count);
    if (!snap_id_count)
        return;
    char path[MAX_PATH];
    mod_path(path, RUN_TMP_FILE);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    EnterCriticalSection(&log_lock);
    HANDLE old = snap_file;
    snap_file = f;
    snap_start_ms = now;
    snap_mgid = 0;
    snap_stream_count = 0;
    snap_guest = guest;
    LeaveCriticalSection(&log_lock);
    if (old != INVALID_HANDLE_VALUE)
        CloseHandle(old);
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    ChrBuilder_t build = (ChrBuilder_t)(base + CHR_BUILDER_RVA);
    uint64_t scratch[8] = {0};
    in_snapshot = true;
    snap_clock(base);
    take_join(base);
    for (int i = 0; i < snap_id_count; i++) {
        uint64_t peer = fake_peer;
        uint32_t id = snap_ids[i];
        snap_mark(0, id);
        build(scratch, &peer, &id);
    }
    snap_mark(0xff, 0);
    char state_tmp[MAX_PATH], state[MAX_PATH];
    mod_path(state_tmp, RUN_STATE_TMP_FILE);
    f = CreateFileA(state_tmp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    EnterCriticalSection(&log_lock);
    state_file = f;
    LeaveCriticalSection(&log_lock);
    take_map_items(base);
    take_position(base);
    take_player(base);
    take_enemies(base);
    take_numbers(base);
    EnterCriticalSection(&log_lock);
    state_file = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&log_lock);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    mod_path(state, RUN_STATE_FILE, (unsigned long long)snap_mgid);
    if (!snap_mgid || !MoveFileExA(state_tmp, state, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(state_tmp);
    in_snapshot = false;
}

// The 0x37 and 0x5e queue-adds call the networked id check FUN_140df0af0 here; it fails for every id in solo.
#define NET_ID_CHECK_RVA 0xdf0af0
static const uint32_t net_id_check_calls[] = {0x4e9858, 0x4e99a8};
typedef bool (*NetIdCheck_t)(void *, uint32_t *);
static NetIdCheck_t real_net_id_check;

static bool hook_net_id_check(void *session, uint32_t *id)
{
    return in_snapshot || real_net_id_check(session, id);
}

// The world update calls the sync manager update (mgr, float dt) here once per frame, solo or online.
#define SYNC_UPDATE_CALL_RVA 0x522994
#define SYNC_UPDATE_FN_RVA 0x4eb650
typedef void (*SyncUpdate_t)(void *, float);
static SyncUpdate_t real_sync_update;
static volatile LONG sync_tick_seen;

// The 0x37 and 0x5e streamers (mgr, float dt) return early while [session+8] is 0, as in solo.
// Each entry is the streamer RVA and its 6-byte je gate.
static const uint32_t streamers[2][2] = {{0x4ec940, 0x4ec9a0}, {0x4ee680, 0x4ee6e3}};
static bool streaming;

// Only the game thread runs the streamers, so the gate is NOPed just for this call.
static void stream_ungated(uint8_t *base, int i, void *mgr, float dt)
{
    static const uint8_t nop6[6] = {0x66, 0x0f, 0x1f, 0x44, 0, 0};
    uint8_t *gate = base + streamers[i][1], saved[6];
    static bool reported[2];
    if (gate[0] != 0x0f || gate[1] != 0x84) {
        if (!reported[i])
            reported[i] = true, log_mismatch(gate, "streamer gate");
        return;
    }
    DWORD old;
    if (!VirtualProtect(gate, 6, PAGE_EXECUTE_READWRITE, &old))
        return;
    memcpy(saved, gate, 6);
    memcpy(gate, nop6, 6);
    FlushInstructionCache(GetCurrentProcess(), gate, 6);
    ((SyncUpdate_t)(base + streamers[i][0]))(mgr, dt);
    memcpy(gate, saved, 6);
    FlushInstructionCache(GetCurrentProcess(), gate, 6);
    VirtualProtect(gate, 6, old, &old);
}

// Closes the snapshot file; a finished snapshot replaces the one for its run, or is dropped outside a run.
static void finish_snapshot(void)
{
    EnterCriticalSection(&log_lock);
    HANDLE f = snap_file;
    snap_file = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&log_lock);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    char tmp[MAX_PATH], dst[MAX_PATH];
    mod_path(tmp, RUN_TMP_FILE);
    mod_path(dst, RUN_FILE, (unsigned long long)snap_mgid);
    // On a host, no 0x37/0x5e means the world is not loaded yet, so the older snapshot stays. A guest streams
    // nothing (it has no sync objects for characters it does not own); the fake host leaves after the 0x42 burst.
    bool ok = snap_mgid && (snap_stream_count || snap_guest);
    if (!ok || !MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
    else
        done_mgid = snap_mgid;
    log_line(ok ? LOG_INFO : LOG_DEBUG, "auto-snapshot mgid=%llu stream=%d guest=%d", (unsigned long long)snap_mgid,
             snap_stream_count, snap_guest);
}

static void hook_sync_update(void *mgr, float dt)
{
    if (!InterlockedExchange(&sync_tick_seen, 1))
        log_line(LOG_DEBUG, "sync-tick mgr=%p", mgr);
    static uint64_t first_tick_ms;
    uint64_t now = unix_ms();
    if (!first_tick_ms)
        first_tick_ms = now;
    // A restored game takes snapshots again once its clock is back.
    // Ground items go back once the fake host is gone (session state no longer 6, guest).
    static bool items_restored;
    uint8_t *ses = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + SESSION_MAN_RVA);
    // A new run started instead of the restore; its marker stays.
    if (restoring_mgid && run_mgid && run_mgid != restoring_mgid) {
        log_line(LOG_INFO, "restore mgid=%llu dropped, new run %llu", (unsigned long long)restoring_mgid,
                 (unsigned long long)run_mgid);
        restoring_mgid = 0;
    }
    if (restoring_mgid && !items_restored && now - first_tick_ms > 10000 && ses && *(int *)(ses + 0xc) != 6) {
        items_restored = true;
        // No session in the Roundtable: the run ended while this player was away, so the game did not rejoin it.
        uint8_t *gdm = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + GAME_DATA_MAN_RVA);
        if (!*(int *)(ses + 0xc) && gdm && gdm[0xdc] == 1) {
            log_line(LOG_INFO, "restore mgid=%llu ended in the Roundtable", (unsigned long long)restoring_mgid);
            restoring_mgid = 0;
            relay_abandon();
            clear_marker("run gone");
        } else {
            restore_snapshot(restoring_mgid);
            if (boss_drops)
                restore_player_number((uint8_t *)GetModuleHandleA(NULL), ses);
            relay_ready();
        }
    }
    static uint64_t last_grace_ms;
    if (graces && items_restored && now - last_grace_ms > 1000)
        last_grace_ms = now, fix_graces((uint8_t *)GetModuleHandleA(NULL));
    if (items_restored)
        restore_enemies((uint8_t *)GetModuleHandleA(NULL), now);
    if (restoring_mgid && now - first_tick_ms > resume_snapshots_ms)
        apply_restore_clock();
    if (!streaming && helper_trampoline) {
        bool early = run_request && InterlockedExchange(&run_request, 0);
        // No snapshots in the Roundtable (mode byte 1 outside a run; guests in a run have it too).
        uint8_t *gdm = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + GAME_DATA_MAN_RVA);
        bool hub = gdm && gdm[0xdc] == 1 && !run_mgid;
        static bool boss;
        static uint64_t last_probe_ms;
        uint8_t *gb = (uint8_t *)GetModuleHandleA(NULL);
        if (now - last_probe_ms > 1000)
            last_probe_ms = now, world_probe(gb);
        // A restore lands before the boss phase with stop_before_boss_ms left for the teammates to join.
        int64_t eta = boss_eta_ms(gb, now);
        bool phase = boss_phase(gb), b = phase || (eta >= 0 && (uint64_t)eta < stop_before_boss_ms);
        // The first snapshot after a boss phase (the next day) runs at once.
        if (b != boss && !b)
            last_auto_ms = 0;
        if (b != boss)
            boss = b, log_line(LOG_INFO, !b ? "snapshots resumed" : phase ? "snapshots paused, boss phase"
                                                     : "snapshots paused, boss in %lld s", (long long)eta / 1000);
        if ((early || now - last_auto_ms > snapshot_every_ms) && !restoring_mgid && !hub && !boss) {
            last_auto_ms = unix_ms();
            take_snapshot();
            streaming = snap_id_count > 0;
        }
    }
    real_sync_update(mgr, dt);
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    uint8_t *session = *(uint8_t **)(base + SESSION_MAN_RVA);
    if (!streaming || !session)
        return;
    uint8_t *m = mgr;
    uint64_t pending37 = *(uint64_t *)(m + 0xcb0), pending5e = *(uint64_t *)(m + 0xce8);
    // Online the game streams the lists itself, also to real peers, so the lists may never drain.
    bool online = *(int *)(session + 8);
    uint64_t age = unix_ms() - snap_start_ms;
    if ((!pending37 && !pending5e) || age > 60000 || (online && age > 20000)) {
        streaming = false;
        log_line(LOG_DEBUG, "stream-done ms=%llu left37=%llu left5e=%llu", (unsigned long long)age,
                 (unsigned long long)pending37, (unsigned long long)pending5e);
        finish_snapshot();
        return;
    }
    if (online)
        return;
    stream_ungated(base, 0, mgr, dt);
    stream_ungated(base, 1, mgr, dt);
}

// Returns false when the sync update hook failed, so no snapshot is ever taken.
bool snapshot_install(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    patch_send_helper();
    real_sync_update = (SyncUpdate_t)(base + SYNC_UPDATE_FN_RVA);
    real_net_id_check = (NetIdCheck_t)(base + NET_ID_CHECK_RVA);
    for (int i = 0; i < 2; i++)
        patch_call(net_id_check_calls[i], NET_ID_CHECK_RVA, (void *)hook_net_id_check, "net id check call");
    return patch_call(SYNC_UPDATE_CALL_RVA, SYNC_UPDATE_FN_RVA, (void *)hook_sync_update, "sync update call");
}
