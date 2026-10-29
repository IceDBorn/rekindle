// Damaged enemies: WorldChrMan +0x17b98 is a vector of every loaded ChrIns. Each record names the chr by its
// field handle (+8) and NpcParam id (+0x68); HP is ChrDataModule ([[chr+0x1b8]]) +0x140, max +0x144.
#include "snapshot.h"
#include <string.h>

#define ENEMY_VT 0x2c18120
#pragma pack(push, 1)
struct enemy_hp { uint64_t handle; int32_t npc, hp, max; };
#pragma pack(pop)
static struct enemy_hp pending_hp[512];
static int pending_hp_count;
static uint64_t pending_hp_until;
static bool pending_missed[512];

static int32_t *enemy_hp_of(uint8_t *chr)
{
    uint8_t **bag = *(uint8_t ***)(chr + 0x1b8);
    return bag && *bag ? (int32_t *)(*bag + 0x140) : NULL;
}

void take_enemies(uint8_t *base)
{
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA);
    if (!wcm)
        return;
    static struct enemy_hp out[512];
    int n = 0;
    for (uint8_t **c = *(uint8_t ***)(wcm + 0x17b98), **end = *(uint8_t ***)(wcm + 0x17ba0); c && c < end && n < 512; c++) {
        int32_t *hp = *c && *(uintptr_t *)*c == (uintptr_t)(base + ENEMY_VT) ? enemy_hp_of(*c) : NULL;
        if (hp && hp[0] > 0 && hp[0] < hp[1])
            out[n++] = (struct enemy_hp){*(uint64_t *)(*c + 8), *(int32_t *)(*c + 0x68), hp[0], hp[1]};
    }
    if (n)
        snap_write(SNAP_ENEMIES, (uint8_t *)out, n * (int)sizeof *out);
    log_line(LOG_INFO, "enemies damaged=%d", n);
}

void queue_enemy_hp(const uint8_t *d, uint32_t l)
{
    if (l % sizeof(struct enemy_hp))
        return;
    pending_hp_count = (int)(l / sizeof(struct enemy_hp)) < 512 ? (int)(l / sizeof(struct enemy_hp)) : 512;
    memcpy(pending_hp, d, pending_hp_count * sizeof(struct enemy_hp));
    memset(pending_missed, 0, sizeof pending_missed);
    pending_hp_until = unix_ms() + 300000;
}

// Enemies load with their map block after the restore, so matching runs every tick for a few minutes.
void restore_enemies(uint8_t *base, uint64_t now)
{
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA);
    if (!pending_hp_count || !wcm)
        return;
    for (uint8_t **c = *(uint8_t ***)(wcm + 0x17b98), **end = *(uint8_t ***)(wcm + 0x17ba0); c && c < end; c++) {
        if (!*c || *(uintptr_t *)*c != (uintptr_t)(base + ENEMY_VT))
            continue;
        uint64_t h = *(uint64_t *)(*c + 8);
        for (int i = 0; i < pending_hp_count; i++) {
            struct enemy_hp *e = &pending_hp[i];
            if (e->npc != *(int32_t *)(*c + 0x68))
                continue;
            int32_t *hp = enemy_hp_of(*c);
            if (e->handle != h || !hp || hp[0] <= 0 || hp[1] <= 0) {
                if (!pending_missed[i] && hp)
                    pending_missed[i] = true, log_line(LOG_DEBUG, "enemy-restore miss npc=%d handle=%016llx/%016llx max=%d/%d",
                                                       e->npc, (unsigned long long)h, (unsigned long long)e->handle, hp[1], e->max);
                continue;
            }
            // Max HP scales with the player count, so the saved share of it goes back.
            int32_t want = e->max > 0 ? (int32_t)((int64_t)e->hp * hp[1] / e->max) : e->hp;
            want = want < 1 ? 1 : want;
            log_line(LOG_INFO, "enemy-restore handle=%016llx npc=%d hp=%d->%d/%d (saved %d/%d)", (unsigned long long)h,
                     e->npc, hp[0], want < hp[0] ? want : hp[0], hp[1], e->hp, e->max);
            if (want < hp[0])
                hp[0] = want;
            pending_missed[i] = pending_missed[pending_hp_count - 1];
            pending_hp[i--] = pending_hp[--pending_hp_count];
        }
    }
    if (pending_hp_count && now > pending_hp_until) {
        log_line(LOG_WARN, "enemy-restore gave up on %d enemies", pending_hp_count);
        pending_hp_count = 0;
    }
}
