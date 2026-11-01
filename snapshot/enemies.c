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

#define BIG_ENEMY_MAX_HP 2000
bool big_enemy_hurt(uint8_t *base)
{
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA);
    for (uint8_t **c = wcm ? *(uint8_t ***)(wcm + 0x17b98) : NULL, **end = wcm ? *(uint8_t ***)(wcm + 0x17ba0) : NULL; c && c < end; c++) {
        int32_t *hp = *c && *(uintptr_t *)*c == (uintptr_t)(base + ENEMY_VT) ? enemy_hp_of(*c) : NULL;
        if (hp && hp[1] >= BIG_ENEMY_MAX_HP && hp[0] < hp[1])
            return true;
    }
    return false;
}

// PROBE: logs when a big enemy appears, first takes damage, dies and unloads, to date boss deaths and fight starts.
void boss_probe(uint8_t *base)
{
    static struct { uint64_t handle; int32_t npc, hp, max; bool hurt, dead, seen; } big[64];
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA);
    if (!wcm)
        return;
    for (int i = 0; i < 64; i++)
        big[i].seen = false;
    for (uint8_t **c = *(uint8_t ***)(wcm + 0x17b98), **end = *(uint8_t ***)(wcm + 0x17ba0); c && c < end; c++) {
        int32_t *hp = *c && *(uintptr_t *)*c == (uintptr_t)(base + ENEMY_VT) ? enemy_hp_of(*c) : NULL;
        if (!hp || hp[1] < BIG_ENEMY_MAX_HP)
            continue;
        uint64_t h = *(uint64_t *)(*c + 8);
        int32_t npc = *(int32_t *)(*c + 0x68);
        int i = 0, free_slot = -1;
        for (; i < 64 && !(big[i].handle == h && big[i].npc == npc); i++)
            if (free_slot < 0 && !big[i].handle)
                free_slot = i;
        if (i == 64) {
            if (free_slot < 0)
                continue;
            i = free_slot;
            big[i].handle = h, big[i].npc = npc, big[i].hurt = big[i].dead = false, big[i].max = 0;
            log_line(LOG_DEBUG, "probe big-enemy appear npc=%d handle=%016llx hp=%d/%d", npc, (unsigned long long)h, hp[0], hp[1]);
        }
        if (big[i].max && big[i].max != hp[1])
            log_line(LOG_DEBUG, "probe big-enemy max npc=%d handle=%016llx %d->%d", npc, (unsigned long long)h, big[i].max, hp[1]);
        big[i].seen = true, big[i].hp = hp[0], big[i].max = hp[1];
        if (!big[i].hurt && hp[0] < hp[1]) {
            big[i].hurt = true;
            log_line(LOG_DEBUG, "probe big-enemy hurt npc=%d handle=%016llx hp=%d/%d", npc, (unsigned long long)h, hp[0], hp[1]);
        }
        if (!big[i].dead && hp[0] <= 0) {
            big[i].dead = true;
            log_line(LOG_DEBUG, "probe big-enemy dead npc=%d handle=%016llx max=%d", npc, (unsigned long long)h, hp[1]);
        }
    }
    for (int i = 0; i < 64; i++)
        if (big[i].handle && !big[i].seen) {
            log_line(LOG_DEBUG, "probe big-enemy gone npc=%d handle=%016llx hp=%d/%d", big[i].npc,
                     (unsigned long long)big[i].handle, big[i].hp, big[i].max);
            big[i].handle = 0;
        }
}

// PROBE: logs the session member list and the player's number, to date what enemy scaling follows.
void party_probe(uint8_t *base)
{
    static int last_n = -1, last_no = -1;
    uint8_t *session = *(uint8_t **)(base + SESSION_MAN_RVA);
    uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA), *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    if (!session || !pgd)
        return;
    uint8_t *m = *(uint8_t **)(session + 0x78), *end = *(uint8_t **)(session + 0x80);
    int n = m && end > m ? (int)((end - m) / 0xf0) : 0, no = *(int *)(pgd + 8);
    if (n == last_n && no == last_no)
        return;
    last_n = n, last_no = no;
    log_line(LOG_INFO, "probe party members=%d number=%d state=%d", n, no, *(int *)(session + 0xc));
    for (int i = 0; i < n && i < 8; i++, m += 0xf0)
        log_line(LOG_DEBUG, "probe party member %d id=%llx no=%d self=%d", i, *(unsigned long long *)(m + 0x10),
                 *(int *)(m + 0xd4), m[0xe1]);
}

void queue_enemy_hp(const uint8_t *d, uint32_t l)
{
    if (l % sizeof(struct enemy_hp))
        return;
    pending_hp_count = (int)(l / sizeof(struct enemy_hp)) < 512 ? (int)(l / sizeof(struct enemy_hp)) : 512;
    memcpy(pending_hp, d, pending_hp_count * sizeof(struct enemy_hp));
    memset(pending_missed, 0, sizeof pending_missed);
    pending_hp_until = unix_ms() + 300000;
    log_line(LOG_INFO, "enemy-restore queued %d", pending_hp_count);
    for (int i = 0; i < pending_hp_count; i++)
        log_line(LOG_DEBUG, "enemy-restore queued npc=%d handle=%016llx saved %d/%d", pending_hp[i].npc,
                 (unsigned long long)pending_hp[i].handle, pending_hp[i].hp, pending_hp[i].max);
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
