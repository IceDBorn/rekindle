// Rekindle snapshots: takes a host-side snapshot of each run every few seconds to p2p_snapshot_run_<mgid>.bin,
// and after a restore puts back ground items, runes, the player number, graces and the day clock.
#include <windows.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int32_t HSteamUser;
typedef HSteamUser (*GetHSteamUser_t)(void);
typedef void *(*FindOrCreateUserInterface_t)(HSteamUser, const char *);

static CRITICAL_SECTION log_lock;
static HMODULE self;

static uint64_t unix_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000;
}

// Log levels from [log] level in rekindle.ini; the file is created on the first line written.
enum { LOG_OFF, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };
static const char *const level_names[] = {"OFF", "ERROR", "WARN", "INFO", "DEBUG"};
static int log_level = LOG_ERROR;
static char log_path[MAX_PATH];
static FILE *log_out;
static bool log_failed;

static void log_init(const char *ini)
{
    char v[16];
    GetPrivateProfileStringA("log", "level", "error", v, sizeof v, ini);
    for (int i = 0; i < 5; i++)
        if (!_stricmp(v, level_names[i]))
            log_level = i;
}

static void log_line(int level, const char *fmt, ...)
{
    if (level > log_level || !log_path[0])
        return;
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&log_lock);
    if (!log_out && !log_failed)
        log_failed = !(log_out = fopen(log_path, "w"));
    if (log_out) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(log_out, "%02d:%02d:%02d.%03d %-5s ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                level_names[level]);
        vfprintf(log_out, fmt, ap);
        fputc('\n', log_out);
        fflush(log_out);
    }
    LeaveCriticalSection(&log_lock);
    va_end(ap);
}

static void log_mismatch(const void *at, const char *name)
{
    log_line(LOG_ERROR, "Patch failed: %s at rva=0x%llx", name,
             (unsigned long long)((uintptr_t)at - (uintptr_t)GetModuleHandleA(NULL)));
}

static void *alloc_near(uint8_t *base)
{
    for (uint8_t *p = base - 0x10000000; p > base - 0x70000000; p -= 0x10000) {
        void *m = VirtualAlloc(p, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (m)
            return m;
    }
    return NULL;
}

// Points the call rel32 at call_rva to hook through a stub near the image, if it still calls fn_rva.
static bool patch_call(uint32_t call_rva, uint32_t fn_rva, void *hook, const char *name)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    uint8_t *call = base + call_rva;
    int32_t rel;
    memcpy(&rel, call + 1, 4);
    if (call[0] != 0xe8 || call + 5 + rel != base + fn_rva) {
        log_mismatch(call, name);
        return false;
    }
    uint8_t *stub = alloc_near(base);
    if (!stub)
        return false;
    stub[0] = 0x48, stub[1] = 0xb8;
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xff, stub[11] = 0xe0;
    rel = (int32_t)(stub - (call + 5));
    DWORD old;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(call + 1, &rel, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

// Snapshot: the game's CHR builder syncs every live networked character id to a fake peer,
// and what the game sends that peer is captured.
#define SEND_HELPER_RVA 0xdf2380
#define CHR_BUILDER_RVA 0xde08c0
typedef void (*SendHelper_t)(void *, uint64_t *, uint32_t, const uint8_t *, int);
typedef int (*ChrBuilder_t)(void *, uint64_t *, uint32_t *);
static SendHelper_t helper_trampoline;
static uint8_t fake_peer_obj[0x1000];
static uint64_t fake_peer = (uint64_t)(uintptr_t)fake_peer_obj;
static uint32_t snap_ids[512];
static int snap_id_count;
static uint64_t snap_start_ms;
static HANDLE snap_file = INVALID_HANDLE_VALUE;
static char dll_dir[MAX_PATH];
static int dll_dir_len;
static __thread bool in_snapshot;
// Auto snapshots go to a temp file and are renamed to p2p_snapshot_run_<multi_game_id>.bin when complete.
// Snapshot timing and restore switches, read from rekindle.ini once the DLL folder is known.
static uint64_t first_snapshot_ms = 10000, snapshot_every_ms = 30000, resume_snapshots_ms = 60000;
static bool restore_items = true, restore_runes = true, boss_drops = true, graces = true;
static uint64_t snap_mgid, last_auto_ms, done_mgid;
// Set while rekindle.dll's run marker names a run that has no finished snapshot yet.
static volatile LONG run_request;

// Snapshot record: u32 ms since trigger, u8 type, u32 len, bytes.
static void snap_write(uint32_t type, const uint8_t *data, int len)
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
    WriteFile(snap_file, hdr, sizeof hdr, &w, NULL);
    WriteFile(snap_file, data, l, &w, NULL);
    LeaveCriticalSection(&log_lock);
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
static __thread bool in_join_build;

static void hook_broadcast(void *mgr, uint32_t type, const uint8_t *data, int len)
{
    if (in_join_build) {
        snap_write(type, data, len);
        return;
    }
    broadcast_trampoline(mgr, type, data, len);
}

// Replaces a position-independent prologue of n >= 14 bytes with an absolute jump; returns the trampoline.
static void *patch_prologue(uint32_t rva, const uint8_t *expect, int n, void *target, const char *name)
{
    uint8_t *fn = (uint8_t *)GetModuleHandleA(NULL) + rva;
    if (memcmp(fn, expect, n)) {
        log_mismatch(fn, name);
        return NULL;
    }
    uint8_t *tramp = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return NULL;
    memcpy(tramp, fn, n);
    tramp[n] = 0xff, tramp[n + 1] = 0x25;
    memset(tramp + n + 2, 0, 4);
    uint8_t *back = fn + n;
    memcpy(tramp + n + 6, &back, 8);
    uint8_t jmp[14] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(jmp + 6, &target, 8);
    DWORD old;
    if (!VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old))
        return NULL;
    memcpy(fn, jmp, 14);
    VirtualProtect(fn, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 14);
    return tramp;
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
    uint8_t *session = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + 0x3c21ec8);
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
    uint8_t *wcm = *(uint8_t **)(base + 0x3c0f0a8);
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

// Marker record (type 0, 5 bytes: kind, id) naming the request the following records answer.
// Kind 0 is the guest's 0x42 kind 0, 0x10 and up are 0x51 kinds + 0x10, 0xff ends the build.
static void snap_mark(uint8_t kind, uint32_t id)
{
    uint8_t rec[14];
    uint32_t rel = (uint32_t)(unix_ms() - snap_start_ms), l = 5;
    memcpy(rec, &rel, 4);
    rec[4] = 0;
    memcpy(rec + 5, &l, 4);
    rec[9] = kind;
    memcpy(rec + 10, &id, 4);
    DWORD w;
    EnterCriticalSection(&log_lock);
    if (snap_file != INVALID_HANDLE_VALUE)
        WriteFile(snap_file, rec, sizeof rec, &w, NULL);
    LeaveCriticalSection(&log_lock);
}

// The host's per-guest join step FUN_140dcdc60 answers each 0x51 kind with one of these builders.
typedef void (*JoinFn_t)(void *, uint64_t *, uint64_t);
static const struct {
    uint8_t kind;
    uint32_t rva, obj;
} join_builders[] = {
    {0, 0xdf23d0, 0x3c21ec8}, {1, 0xdf24a0, 0x3c21ec8}, {2, 0xddee70, 0}, {3, 0xdf2410, 0x3c21ec8},
    {6, 0xddf190, 0}, {7, 0x65efb0, 0x3c12648}, {10, 0x35c740, 0x3c0ae30}, {11, 0x5785a0, 0x3c10b68},
};

// WorldAreaTime (StormTimer finds the same global): the day clock is a packed qword at +0x100,
// hour at bit 34, minute at 39, second at 45.
#define WORLD_AREA_TIME_RVA 0x3c13158
static uint64_t restore_mgid, restore_clock;

// Clock record: type 0, len 8 + 0x200: the clock qword, then the WorldAreaTime struct for later study.
static void snap_clock(uint8_t *base)
{
    uint8_t *w = *(uint8_t **)(base + WORLD_AREA_TIME_RVA);
    if (!w)
        return;
    uint8_t rec[9 + 8 + 0x200];
    uint32_t rel = (uint32_t)(unix_ms() - snap_start_ms), l = 8 + 0x200;
    memcpy(rec, &rel, 4);
    rec[4] = 0;
    memcpy(rec + 5, &l, 4);
    memcpy(rec + 9, w + 0x100, 8);
    memcpy(rec + 17, w, 0x200);
    DWORD wr;
    EnterCriticalSection(&log_lock);
    if (snap_file != INVALID_HANDLE_VALUE)
        WriteFile(snap_file, rec, sizeof rec, &wr, NULL);
    LeaveCriticalSection(&log_lock);
}

// Kind 12: ChrNonActiveBlockManager state for unloaded blocks, queued by FUN_140e0e580 into a
// 0x28-byte queue (CSInitSyncStep+0xc8) and drained by FUN_140e0ee20 under a byte budget at +0x20.
typedef void *(*Alloc_t)(uint64_t size, uint32_t align, void *allocator);
typedef void *(*Ctor_t)(void *obj);
typedef void (*NabFn_t)(void *queue, void *arg);
static uint8_t *nab_queue;

static void take_nonactive(uint8_t *base)
{
    if (!*(uint8_t *)(base + 0x3c196f2) || !*(void **)(base + 0x3c0f3a8))
        return;
    if (!nab_queue) {
        nab_queue = ((Alloc_t)(base + 0x214e8d0))(0x28, 8, *(void **)(base + 0x3c2da80));
        if (!nab_queue)
            return;
        ((Ctor_t)(base + 0xe0dfe0))(nab_queue);
    }
    uint64_t peer = fake_peer;
    float dt[4] = {0};
    log_line(LOG_DEBUG, "join-build kind=12 rva=0xe0e580");
    snap_mark(0x10 + 12, 0);
    ((NabFn_t)(base + 0xe0e580))(nab_queue, &peer);
    int n = 0;
    while (*(uint64_t *)(nab_queue + 8) != *(uint64_t *)(nab_queue + 0x10) && n++ < 500) {
        *(int32_t *)(nab_queue + 0x20) = 0;
        ((NabFn_t)(base + 0xe0ee20))(nab_queue, dt);
    }
    log_line(LOG_DEBUG, "nonactive drained=%d left=%d", n,
             (int)((*(uint64_t *)(nab_queue + 0x10) - *(uint64_t *)(nab_queue + 8)) / 0x28));
}

// Restored game: put the snapshot's clock back once it has been in-world a minute.
static uint64_t first_tick_ms;

// Read at the first sync tick: rekindle.dll's auto restore writes the file after this DLL loads.
static void read_restore_clock(void)
{
    char rc_path[MAX_PATH];
    snprintf(rc_path, MAX_PATH, "%.*srestore_clock.txt", dll_dir_len, dll_dir);
    FILE *rc = fopen(rc_path, "r");
    if (rc) {
        unsigned long long m = 0, c = 0;
        if (fscanf(rc, "%llu %llx", &m, &c) == 2)
            restore_mgid = m, restore_clock = c;
        fclose(rc);
    }
}

static void apply_restore_clock(void)
{
    uint8_t *w = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + WORLD_AREA_TIME_RVA);
    if (w) {
        uint64_t was = *(uint64_t *)(w + 0x100);
        *(uint64_t *)(w + 0x100) = restore_clock;
        *(uint64_t *)(w + 0x110) = restore_clock;
        log_line(LOG_INFO, "restore-clock mgid=%llu was=%llx now=%llx", (unsigned long long)restore_mgid,
                 (unsigned long long)was, (unsigned long long)restore_clock);
    }
    restore_mgid = 0;
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%.*srestore_clock.txt", dll_dir_len, dll_dir);
    DeleteFileA(path);
}

// Kind 8 (0x6d): the step FUN_140dcdc60 packs h/m/s of [0x143c125d8]+8 and of the day clock into two u32s.
static uint32_t pack_hms(uint64_t q)
{
    return (uint32_t)(q >> 34 & 0x1f) | (uint32_t)(q >> 39 & 0x3f) << 5 | ((uint32_t)(q >> 45 & 0x3f) | 0x40) << 11;
}

static void take_join_dates(uint8_t *base)
{
    uint64_t *d = *(uint64_t **)(base + 0x3c125d8);
    uint8_t *w = *(uint8_t **)(base + WORLD_AREA_TIME_RVA);
    if (!d || !w)
        return;
    uint32_t v[2] = {pack_hms(d[1]) | (uint32_t)(base[0x3c125d2] & 1) << 18, pack_hms(*(uint64_t *)(w + 0x100))};
    log_line(LOG_DEBUG, "join-build kind=8 dates=%08x %08x", v[0], v[1]);
    snap_mark(0x10 + 8, 0);
    snap_write(0x6d, (uint8_t *)v, 8);
}

static void take_join(uint8_t *base)
{
    uint64_t scratch[16] = {0};
    // The join answers only run as an online host (session state 3); fake it for the builders only.
    uint8_t *session = *(uint8_t **)(base + 0x3c21ec8);
    int32_t state = session ? *(int32_t *)(session + 0xc) : 0;
    if (session)
        *(int32_t *)(session + 0xc) = 3;
    in_join_build = true;
    for (size_t i = 0; i < sizeof join_builders / sizeof *join_builders; i++) {
        void *obj = scratch;
        if (join_builders[i].obj && !(obj = *(void **)(base + join_builders[i].obj)))
            continue;
        if (join_builders[i].rva == 0x65efb0 && !((uint8_t *)obj)[0x100])
            continue;
        if (join_builders[i].rva == 0xddee70 && !*(void **)(base + 0x3c115a8))
            continue;
        uint64_t peer = fake_peer;
        log_line(LOG_DEBUG, "join-build kind=%d rva=0x%x", join_builders[i].kind, join_builders[i].rva);
        snap_mark(0x10 + join_builders[i].kind, 0);
        ((JoinFn_t)(base + join_builders[i].rva))(obj, &peer, 0);
    }
    uint8_t *net = *(uint8_t **)(base + 0x3c046a8);
    // Kind 4 runs on a host only when [obj+0x79] is set, which a solo game never sets; a guest still waits for it.
    uint8_t *k4 = net ? *(uint8_t **)(net + 0x48) : NULL;
    if (k4) {
        log_line(LOG_DEBUG, "join-build kind=4 rva=0xb12e60 flag=%d", k4[0x79]);
        snap_mark(0x10 + 4, 0);
        ((Ctor_t)(base + 0xb12e60))(k4);
    }
    uint8_t *k5 = net ? *(uint8_t **)(net + 0x50) : NULL;
    if (k5 && k5[0x89]) {
        log_line(LOG_DEBUG, "join-build kind=5 rva=0xb79030");
        snap_mark(0x10 + 5, 0);
        ((Ctor_t)(base + 0xb79030))(k5);
    }
    in_join_build = false;
    take_join_dates(base);
    take_nonactive(base);
    if (session)
        *(int32_t *)(session + 0xc) = state;
}

// Ground items: MapItemMan keeps five lists (types 1, 3, 2, 4, 6), record at node+0x20. The join answer 0x13
// skips unsynced records and the restored guest never asks for it, so snapshots keep every record as type 0xf1.
#define MAP_ITEM_MAN_RVA 0x3c10b68
#define SNAP_MAP_ITEM 0xf1
static const int item_heads[5] = {0x38, 0x48, 0x58, 0x68, 0x78};
typedef int64_t (*ItemWrite_t)(void *rec, void *writer);
typedef void (*ItemRead_t)(void *rec, void *reader);
typedef char (*ItemValid_t)(void *rec);
typedef void *(*ItemFind_t)(void *man, uint32_t id);
typedef void *(*ItemInsert_t)(void *man, void *rec);
typedef void (*ArrayDtor_t)(void *p, uint64_t size, uint64_t count, void *dtor);

static void take_map_items(uint8_t *base)
{
    uint8_t *man = *(uint8_t **)(base + MAP_ITEM_MAN_RVA), buf[1024];
    int count[5] = {0};
    for (int i = 0; man && i < 5; i++)
        for (uint8_t *n = *(uint8_t **)(man + item_heads[i]); n; n = *(uint8_t **)n) {
            struct { uint8_t *cur; int64_t cap; } w = {buf, sizeof buf};
            int64_t len = ((ItemWrite_t)(base + 0x574320))(n + 0x20, &w);
            if (len > 0 && len <= (int64_t)sizeof buf)
                snap_write(SNAP_MAP_ITEM, buf, (int)len), count[i]++;
        }
    log_line(LOG_INFO, "map-items t1=%d t3=%d t2=%d t4=%d t6=%d", count[0], count[1], count[2], count[3], count[4]);
}

// Rejoin position as the save stores it (u32 tile, tile-local x y z, yaw): GameMan keeps the oldest of the
// last five grounded positions; before the first one, the player's own tile-local position is used.
#define SNAP_POSITION 0xf2
typedef void *(*ChrGet_t)(void *chr, void *out);
static void take_position(uint8_t *base)
{
    uint8_t *gm = *(uint8_t **)(base + 0x3c13258), *wcm = *(uint8_t **)(base + 0x3c0f0a8), out[20];
    uint8_t *pl = wcm ? *(uint8_t **)(wcm + 0x174e8) : NULL;
    uint32_t tile = 0xffffffff;
    float pos[3], rot[3];
    int src = 0;
    if (pl && *(void **)(pl + 0x5f0))
        ((ChrGet_t)(base + 0x66b700))(pl, &tile);
    if (gm && gm[0xc80] && *(uint32_t *)(gm + 0xc6c) != 0xffffffff) {
        memcpy(out, gm + 0xc6c, 4), memcpy(out + 4, gm + 0xc60, 12), memcpy(out + 16, gm + 0xc74, 4), src = 1;
    } else if (tile != 0xffffffff) {
        ((ChrGet_t)(base + 0x66b740))(pl, pos), ((ChrGet_t)(base + 0x66b680))(pl, rot);
        memcpy(out, &tile, 4), memcpy(out + 4, pos, 12), memcpy(out + 16, &rot[1], 4), src = 2;
    }
    if (src)
        snap_write(SNAP_POSITION, out, 20);
    log_line(LOG_INFO, "position src=%d tile=%08x", src, src ? *(uint32_t *)out : 0);
}

// Field boss camps share one grace (chr 4xxx0500, asset 4xxx1500) between boss variants. On reload the killed
// variant registers it, but each unkilled variant's event disables it again, so re-enable registered ones.
typedef void (*ChrEnable_t)(uint32_t *id, bool disable);
typedef void (*AssetEnable_t)(uint32_t *id, bool enable, int mode);
static uint32_t graces_logged[64];
static void fix_grace_node(uint8_t *base, uint8_t *n, int depth)
{
    if (!n || n[0x19] || depth > 64)
        return;
    fix_grace_node(base, *(uint8_t **)n, depth + 1);
    uint32_t asset = *(uint32_t *)(n + 0x20), chr = asset - 1000;
    uint8_t *c = *(uint8_t **)(n + 0x28);
    if (c && *(uint32_t *)(c + 0x20) == asset && c[0x3c] && asset % 10000 == 1500 && asset / 1000000 >= 45 &&
        asset / 1000000 <= 46) {
        ((ChrEnable_t)(base + 0x5f06e0))(&chr, false);
        ((AssetEnable_t)(base + 0x5f46f0))(&asset, true, 1);
        int i = 0;
        while (i < 64 && graces_logged[i] && graces_logged[i] != asset)
            i++;
        if (i < 64 && !graces_logged[i])
            graces_logged[i] = asset, log_line(LOG_INFO, "grace-fix asset=%u", asset);
    }
    fix_grace_node(base, *(uint8_t **)(n + 0x10), depth + 1);
}

static void fix_graces(uint8_t *base)
{
    uint8_t *m = *(uint8_t **)(base + 0x3c10f60), *mm = m ? *(uint8_t **)m : NULL;
    uint8_t *head = mm ? *(uint8_t **)(mm + 0x40) : NULL;
    if (head)
        fix_grace_node(base, *(uint8_t **)(head + 8), 0);
}

// Level and runes (PlayerGameData +0x68 level, +0x6c held runes, +0x110 runes gained). The save can be older
// than the snapshot, so a restore gives back runes earned after the game last wrote it.
#define SNAP_PLAYER 0xf3
static void take_player(uint8_t *base)
{
    uint8_t *gdm = *(uint8_t **)(base + 0x3c078d0), *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    if (!pgd)
        return;
    int32_t v[3] = {*(int32_t *)(pgd + 0x68), *(int32_t *)(pgd + 0x6c), *(int32_t *)(pgd + 0x110)};
    snap_write(SNAP_PLAYER, (uint8_t *)v, sizeof v);
    log_line(LOG_INFO, "player level=%d runes=%d gained=%d", v[0], v[1], v[2]);
}

static void restore_player(uint8_t *base, const int32_t v[3])
{
    uint8_t *gdm = *(uint8_t **)(base + 0x3c078d0), *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    if (!pgd)
        return;
    int32_t *lv = (int32_t *)(pgd + 0x68), *runes = (int32_t *)(pgd + 0x6c), *gained = (int32_t *)(pgd + 0x110);
    log_line(LOG_INFO, "player-restore level=%d/%d runes=%d/%d gained=%d/%d", *lv, v[0], *runes, v[1], *gained, v[2]);
    if (*lv == v[0] && v[2] > *gained)
        *runes += v[2] - *gained, *gained = v[2];
}

// Puts the crashed run's ground items back from its snapshot, skipping ids already in the world.
static void restore_map_items(uint64_t mgid)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL), *man = *(uint8_t **)(base + MAP_ITEM_MAN_RVA);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%.*sp2p_snapshot_run_%llu.bin", dll_dir_len, dll_dir, (unsigned long long)mgid);
    FILE *f = man ? fopen(path, "rb") : NULL;
    if (!f) {
        log_line(man ? LOG_ERROR : LOG_WARN, "map-items-restore mgid=%llu: %s", (unsigned long long)mgid,
                 man ? "no snapshot" : "no MapItemMan");
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    uint8_t *s = malloc(n > 0 ? n : 1);
    fseek(f, 0, SEEK_SET);
    n = s ? (long)fread(s, 1, n, f) : 0;
    fclose(f);
    // MapItemMan id counter offset per item type, raised past every restored id.
    static const int counters[7] = {0, 0x08, 0x10, 0x0c, 0x14, 0, 0x18};
    int added = 0, skipped = 0, bad = 0;
    for (long o = 0; o + 9 <= n;) {
        uint32_t l;
        memcpy(&l, s + o + 5, 4);
        if (o + 9 + (long)l > n)
            break;
        uint8_t ty = s[o + 4], *d = s + o + 9;
        o += 9 + l;
        if (ty == SNAP_PLAYER && l == 12 && restore_runes) {
            int32_t v[3];
            memcpy(v, d, 12), restore_player(base, v);
        }
        if (ty != SNAP_MAP_ITEM || !restore_items)
            continue;
        __attribute__((aligned(16))) uint8_t rec[0xd0];
        struct { uint8_t *cur; int32_t left; uint8_t err; } r = {d, (int32_t)l, 0};
        ((Ctor_t)(base + 0x56e440))(rec);
        ((ItemRead_t)(base + 0x571580))(rec, &r);
        uint32_t id = *(uint32_t *)(rec + 0x58);
        if (r.err || !((ItemValid_t)(base + 0x573100))(rec))
            bad++;
        else if (((ItemFind_t)(base + 0x582680))(man, id))
            skipped++;
        else {
            int t = id & 15;
            if (t < 7 && counters[t] && *(uint32_t *)(man + counters[t]) <= id >> 5)
                *(uint32_t *)(man + counters[t]) = (id >> 5) + 1;
            ((ItemInsert_t)(base + 0x57d0a0))(man, rec);
            added++;
        }
        ((ArrayDtor_t)(base + 0x2690a5c))(rec + 8, 0x50, 1, base + 0x56fac0);
    }
    free(s);
    uint8_t *session = *(uint8_t **)(base + 0x3c21ec8);
    log_line(skipped || bad ? LOG_WARN : LOG_INFO, "map-items-restore mgid=%llu added=%d skipped=%d bad=%d state=%d",
             (unsigned long long)mgid, added, skipped, bad, session ? *(int *)(session + 0xc) : -1);
}

// A restored player keeps the session player number 2 it got from the fake host (player 1), and only sees
// ground items targeted at its own number. The game's leave path resets it with FUN_14066f2d0(PlayerIns, 1).
typedef void (*SetPlayerNo_t)(void *player, int no);
typedef char (*ItemVisible_t)(void *rec);

static void become_player_one(uint8_t *base, uint8_t *session)
{
    uint8_t *gdm = *(uint8_t **)(base + 0x3c078d0), *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    uint8_t *wcm = *(uint8_t **)(base + 0x3c0f0a8), *player = wcm ? *(uint8_t **)(wcm + 0x174e8) : NULL;
    int was = pgd ? *(int *)(pgd + 8) : -1;
    if (!pgd || !player || was == 1)
        return;
    ((SetPlayerNo_t)(base + 0x66f2d0))(player, 1);
    // The self member entry (+0xe1 set) keeps its number at +0xd4 for the number allocator.
    for (uint8_t *m = *(uint8_t **)(session + 0x78), *end = *(uint8_t **)(session + 0x80); m && m < end; m += 0xf0)
        if (m[0xe1])
            *(int *)(m + 0xd4) = 1;
    // Items already on the ground got no visual; +0xf8 makes the manager update retry it.
    uint8_t *man = *(uint8_t **)(base + MAP_ITEM_MAN_RVA);
    int shown = 0;
    for (int i = 0; man && i < 5; i++)
        for (uint8_t *n = *(uint8_t **)(man + item_heads[i]); n; n = *(uint8_t **)n)
            if (((ItemVisible_t)(base + 0x572c10))(n + 0x20))
                n[0xf8] = 1, shown++;
    log_line(LOG_INFO, "player-number was=%d now=%d retried=%d", was, *(int *)(pgd + 8), shown);
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
    snprintf(path, MAX_PATH, "%.*sp2p_snapshot_auto.tmp", dll_dir_len, dll_dir);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    EnterCriticalSection(&log_lock);
    HANDLE old = snap_file;
    snap_file = f;
    snap_start_ms = now;
    snap_mgid = 0;
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
    snprintf(tmp, MAX_PATH, "%.*sp2p_snapshot_auto.tmp", dll_dir_len, dll_dir);
    snprintf(dst, MAX_PATH, "%.*sp2p_snapshot_run_%llu.bin", dll_dir_len, dll_dir, (unsigned long long)snap_mgid);
    if (!snap_mgid || !MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
    else
        done_mgid = snap_mgid;
    log_line(snap_mgid ? LOG_INFO : LOG_DEBUG, "auto-snapshot mgid=%llu", (unsigned long long)snap_mgid);
}

static void hook_sync_update(void *mgr, float dt)
{
    if (!InterlockedExchange(&sync_tick_seen, 1))
        log_line(LOG_DEBUG, "sync-tick mgr=%p", mgr);
    uint64_t now = unix_ms();
    if (!first_tick_ms) {
        first_tick_ms = now;
        read_restore_clock();
    }
    // A restored game takes snapshots again once its clock is back.
    // Ground items go back once the fake host is gone (session state no longer 6, guest).
    static bool items_restored;
    uint8_t *ses = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + 0x3c21ec8);
    if (restore_mgid && !items_restored && now - first_tick_ms > 10000 && ses && *(int *)(ses + 0xc) != 6) {
        items_restored = true;
        if (boss_drops)
            become_player_one((uint8_t *)GetModuleHandleA(NULL), ses);
        restore_map_items(restore_mgid);
    }
    static uint64_t last_grace_ms;
    if (graces && items_restored && now - last_grace_ms > 1000)
        last_grace_ms = now, fix_graces((uint8_t *)GetModuleHandleA(NULL));
    if (restore_mgid && now - first_tick_ms > resume_snapshots_ms)
        apply_restore_clock();
    if (!streaming && helper_trampoline) {
        bool early = run_request && InterlockedExchange(&run_request, 0);
        if ((early || now - last_auto_ms > snapshot_every_ms) && !restore_mgid) {
            last_auto_ms = unix_ms();
            take_snapshot();
            streaming = snap_id_count > 0;
        }
    }
    real_sync_update(mgr, dt);
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    uint8_t *session = *(uint8_t **)(base + 0x3c21ec8);
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
// Polls run_active.txt once a second: a new run gets its first snapshot first_snapshot_ms after the
// marker appears, retried every 10 s until one is finished.
static DWORD WINAPI marker_thread(LPVOID unused)
{
    (void)unused;
    uint64_t seen_mgid = 0, seen_ms = 0, asked_ms = 0;
    char marker[MAX_PATH];
    snprintf(marker, MAX_PATH, "%.*srun_active.txt", dll_dir_len, dll_dir);
    for (;; Sleep(1000)) {
        unsigned long long m = 0;
        FILE *f = fopen(marker, "r");
        if (f) {
            if (fscanf(f, "%llu", &m) != 1)
                m = 0;
            fclose(f);
        }
        uint64_t t = unix_ms();
        if (m != seen_mgid)
            seen_mgid = m, seen_ms = t;
        if (m && m != done_mgid && t - seen_ms > first_snapshot_ms && t - asked_ms > 10000)
            asked_ms = t, InterlockedExchange(&run_request, 1);
    }
    return 0;
}

static void patch_game(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    patch_send_helper();
    real_sync_update = (SyncUpdate_t)(base + SYNC_UPDATE_FN_RVA);
    real_net_id_check = (NetIdCheck_t)(base + NET_ID_CHECK_RVA);
    for (int i = 0; i < 2; i++)
        patch_call(net_id_check_calls[i], NET_ID_CHECK_RVA, (void *)hook_net_id_check, "net id check call");
    if (patch_call(SYNC_UPDATE_CALL_RVA, SYNC_UPDATE_FN_RVA, (void *)hook_sync_update, "sync update call"))
        CloseHandle(CreateThread(NULL, 0, marker_thread, NULL, 0, NULL));
}

static void load_settings(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    memcpy(dll_dir, path, n);
    dll_dir_len = (int)n;
    snprintf(path + n, MAX_PATH - n, "rekindle.ini");
    log_init(path);
    first_snapshot_ms = GetPrivateProfileIntA("snapshots", "first_after_s", 10, path) * 1000ull;
    snapshot_every_ms = GetPrivateProfileIntA("snapshots", "every_s", 30, path) * 1000ull;
    resume_snapshots_ms = GetPrivateProfileIntA("snapshots", "resume_after_restore_s", 60, path) * 1000ull;
    restore_items = GetPrivateProfileIntA("restore", "restore_ground_items", 1, path);
    restore_runes = GetPrivateProfileIntA("restore", "restore_runes", 1, path);
    boss_drops = GetPrivateProfileIntA("restore", "fix_boss_drops", 1, path);
    graces = GetPrivateProfileIntA("restore", "fix_graces", 1, path);
    // The log only holds this launch: an old one is removed even when nothing gets written.
    snprintf(log_path, MAX_PATH, "%.*srekindle_snapshot.log", (int)n, path);
    DeleteFileA(log_path);
    log_line(LOG_INFO, "settings first=%llu every=%llu resume=%llu items=%d runes=%d drops=%d graces=%d",
             (unsigned long long)first_snapshot_ms, (unsigned long long)snapshot_every_ms,
             (unsigned long long)resume_snapshots_ms, restore_items, restore_runes, boss_drops, graces);
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
        patch_game();
        return 0;
    }
    log_line(LOG_ERROR, "Steam networking never came up, snapshots are off");
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&log_lock);
        CloseHandle(CreateThread(NULL, 0, install, NULL, 0, NULL));
    }
    return TRUE;
}
