// Join answers: the host's per-guest join step FUN_140dcdc60 answers each 0x51 kind with one of these builders;
// a snapshot runs them all for the fake peer.
#include "snapshot.h"

typedef void (*JoinFn_t)(void *, uint64_t *, uint64_t);
static const struct {
    uint8_t kind;
    uint32_t rva, obj;
} join_builders[] = {
    {0, 0xdf23d0, SESSION_MAN_RVA}, {1, 0xdf24a0, SESSION_MAN_RVA}, {2, 0xddee70, 0}, {3, 0xdf2410, SESSION_MAN_RVA},
    {6, 0xddf190, 0}, {7, 0x65efb0, 0x3c12648}, {10, 0x35c740, 0x3c0ae30}, {11, 0x5785a0, MAP_ITEM_MAN_RVA},
};

// Kind 12: ChrNonActiveBlockManager state for unloaded blocks, queued by FUN_140e0e580 into a
// 0x28-byte queue (CSInitSyncStep+0xc8) and drained by FUN_140e0ee20 under a byte budget at +0x20.
typedef void *(*Alloc_t)(uint64_t size, uint32_t align, void *allocator);
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

void take_join(uint8_t *base)
{
    uint64_t scratch[16] = {0};
    // The join answers only run as an online host (session state 3); fake it for the builders only.
    uint8_t *session = *(uint8_t **)(base + SESSION_MAN_RVA);
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
    uint8_t *net = *(uint8_t **)(base + NET_MAN_RVA);
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
