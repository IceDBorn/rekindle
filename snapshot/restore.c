// After a restore: reads the crashed run's snapshot once the fake host is gone and puts back what the join
// itself does not restore.
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    uint8_t *s = malloc(*n > 0 ? *n : 1);
    fseek(f, 0, SEEK_SET);
    *n = s ? (long)fread(s, 1, *n, f) : 0;
    fclose(f);
    return s;
}

struct restore_counts { int snap_day, added, skipped, bad; };

// own: also apply Rekindle's own records (items, runes, enemy HP); the .state file has newer ones when present.
static void apply_records(uint8_t *base, uint8_t *man, uint8_t *s, long n, bool own, struct restore_counts *c)
{
    for (long o = 0; o + 9 <= n;) {
        uint32_t l;
        memcpy(&l, s + o + 5, 4);
        if (o + 9 + (long)l > n)
            break;
        uint8_t ty = s[o + 4];
        uint8_t *d = s + o + 9;
        o += 9 + l;
        if (ty == 0 && l == 8 + 0x200)
            c->snap_day = d[8 + 0x156];
        if (!own)
            continue;
        if (ty == SNAP_PLAYER && l == 12 && restore_runes) {
            int32_t v[3];
            memcpy(v, d, 12), restore_player(base, v);
        }
        if (ty == SNAP_ENEMIES && enemy_hp)
            queue_enemy_hp(d, l);
        if (ty == SNAP_NUMBERS)
            queue_numbers(d, l);
        if (ty == SNAP_MAP_ITEM && restore_items) {
            int r = restore_map_item(base, man, d, l);
            c->added += r == 0, c->skipped += r == 1, c->bad += r == 2;
        }
    }
}

void restore_snapshot(uint64_t mgid)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL), *man = *(uint8_t **)(base + MAP_ITEM_MAN_RVA);
    char path[MAX_PATH], state_path[MAX_PATH];
    long n = 0, sn = 0;
    mod_path(path, RUN_FILE, (unsigned long long)mgid);
    mod_path(state_path, RUN_STATE_FILE, (unsigned long long)mgid);
    uint8_t *s = man ? read_file(path, &n) : NULL;
    if (!s) {
        log_line(man ? LOG_ERROR : LOG_WARN, "map-items-restore mgid=%llu: %s", (unsigned long long)mgid,
                 man ? "no snapshot" : "no MapItemMan");
        return;
    }
    uint8_t *state = read_file(state_path, &sn);
    struct restore_counts c = {-1, 0, 0, 0};
    apply_records(base, man, s, n, !state, &c);
    if (state)
        apply_records(base, man, state, sn, true, &c);
    free(s), free(state);
    restore_day(base, c.snap_day);
    uint8_t *session = *(uint8_t **)(base + SESSION_MAN_RVA);
    log_line(c.skipped || c.bad ? LOG_WARN : LOG_INFO,
             "map-items-restore mgid=%llu added=%d skipped=%d bad=%d state=%d from=%s", (unsigned long long)mgid,
             c.added, c.skipped, c.bad, session ? *(int *)(session + 0xc) : -1, state ? "state" : "snapshot");
}
