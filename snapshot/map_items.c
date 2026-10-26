// Ground items: MapItemMan keeps five lists (types 1, 3, 2, 4, 6), record at node+0x20. The join answer 0x13
// skips unsynced records and the restored guest never asks for it, so snapshots keep every record as type 0xf1.
#include "snapshot.h"

const int item_heads[5] = {0x38, 0x48, 0x58, 0x68, 0x78};
typedef int64_t (*ItemWrite_t)(void *rec, void *writer);
typedef void (*ItemRead_t)(void *rec, void *reader);
typedef char (*ItemValid_t)(void *rec);
typedef void *(*ItemFind_t)(void *man, uint32_t id);
typedef void *(*ItemInsert_t)(void *man, void *rec);
typedef void (*ArrayDtor_t)(void *p, uint64_t size, uint64_t count, void *dtor);

void take_map_items(uint8_t *base)
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

// Puts one record back unless its id is already in the world: 0 added, 1 skipped, 2 bad.
int restore_map_item(uint8_t *base, uint8_t *man, uint8_t *d, uint32_t l)
{
    // MapItemMan id counter offset per item type, raised past every restored id.
    static const int counters[7] = {0, 0x08, 0x10, 0x0c, 0x14, 0, 0x18};
    __attribute__((aligned(16))) uint8_t rec[0xd0];
    struct { uint8_t *cur; int32_t left; uint8_t err; } r = {d, (int32_t)l, 0};
    int ret = 0;
    ((Ctor_t)(base + 0x56e440))(rec);
    ((ItemRead_t)(base + 0x571580))(rec, &r);
    uint32_t id = *(uint32_t *)(rec + 0x58);
    if (r.err || !((ItemValid_t)(base + 0x573100))(rec))
        ret = 2;
    else if (((ItemFind_t)(base + 0x582680))(man, id))
        ret = 1;
    else {
        int t = id & 15;
        if (t < 7 && counters[t] && *(uint32_t *)(man + counters[t]) <= id >> 5)
            *(uint32_t *)(man + counters[t]) = (id >> 5) + 1;
        ((ItemInsert_t)(base + 0x57d0a0))(man, rec);
    }
    ((ArrayDtor_t)(base + 0x2690a5c))(rec + 8, 0x50, 1, base + 0x56fac0);
    return ret;
}
