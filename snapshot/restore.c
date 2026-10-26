// After a restore: reads the crashed run's snapshot once the fake host is gone and puts back what the join
// itself does not restore.
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void restore_snapshot(uint64_t mgid)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL), *man = *(uint8_t **)(base + MAP_ITEM_MAN_RVA);
    char path[MAX_PATH];
    mod_path(path, RUN_FILE, (unsigned long long)mgid);
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
    int snap_day = -1, added = 0, skipped = 0, bad = 0;
    for (long o = 0; o + 9 <= n;) {
        uint32_t l;
        memcpy(&l, s + o + 5, 4);
        if (o + 9 + (long)l > n)
            break;
        uint8_t ty = s[o + 4], *d = s + o + 9;
        o += 9 + l;
        if (ty == 0 && l == 8 + 0x200)
            snap_day = d[8 + 0x156];
        if (ty == SNAP_PLAYER && l == 12 && restore_runes) {
            int32_t v[3];
            memcpy(v, d, 12), restore_player(base, v);
        }
        if (ty == SNAP_ENEMIES && enemy_hp)
            queue_enemy_hp(d, l);
        if (ty == SNAP_MAP_ITEM && restore_items) {
            int r = restore_map_item(base, man, d, l);
            added += r == 0, skipped += r == 1, bad += r == 2;
        }
    }
    free(s);
    restore_day(base, snap_day);
    uint8_t *session = *(uint8_t **)(base + SESSION_MAN_RVA);
    log_line(skipped || bad ? LOG_WARN : LOG_INFO, "map-items-restore mgid=%llu added=%d skipped=%d bad=%d state=%d",
             (unsigned long long)mgid, added, skipped, bad, session ? *(int *)(session + 0xc) : -1);
}
