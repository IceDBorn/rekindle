// Snapshot: the game's CHR builder syncs every live networked character id to a fake peer,
// and what the game sends that peer is captured. Runs from the sync manager update on the game thread.
#include "snapshot.h"
#include "patch.h"
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
static uint64_t snap_start_ms;
static HANDLE snap_file = INVALID_HANDLE_VALUE;
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

static void hook_send_helper(void *mgr, uint64_t *peer, uint32_t type, const uint8_t *data, int len)
{
    if (peer && *peer == fake_peer) {
        if (type == 0x1e && data && len >= 0x58)
            memcpy(&snap_mgid, data + 0x50, 8);
        snap_write(type, data, len);
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
    broadcast_trampoline(mgr, type, data, len);
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
    collect_live_ids();
    if (!snap_id_count)
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
    take_map_items(base);
    take_position(base);
    take_player(base);
    take_enemies(base);
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
    // Without 0x37/0x5e (world not loaded yet) the fake host never finishes, so keep the older snapshot.
    if (!snap_mgid || !snap_stream_count || !MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
    else
        done_mgid = snap_mgid;
    log_line(snap_mgid && snap_stream_count ? LOG_INFO : LOG_DEBUG, "auto-snapshot mgid=%llu stream=%d",
             (unsigned long long)snap_mgid, snap_stream_count);
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
    if (restoring_mgid && !items_restored && now - first_tick_ms > 10000 && ses && *(int *)(ses + 0xc) != 6) {
        items_restored = true;
        if (boss_drops)
            become_player_one((uint8_t *)GetModuleHandleA(NULL), ses);
        restore_snapshot(restoring_mgid);
        relay_ready();
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
        if ((early || now - last_auto_ms > snapshot_every_ms) && !restoring_mgid) {
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
