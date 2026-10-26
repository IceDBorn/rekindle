// Field boss camps share one grace (chr 4xxx0500, asset 4xxx1500) between boss variants. On reload the killed
// variant registers it, but each unkilled variant's event disables it again, so re-enable registered ones.
#include "snapshot.h"

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

void fix_graces(uint8_t *base)
{
    uint8_t *m = *(uint8_t **)(base + 0x3c10f60), *mm = m ? *(uint8_t **)m : NULL;
    uint8_t *head = mm ? *(uint8_t **)(mm + 0x40) : NULL;
    if (head)
        fix_grace_node(base, *(uint8_t **)(head + 8), 0);
}
