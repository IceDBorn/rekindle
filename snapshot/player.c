// The player: rejoin position, level and runes, and the session player number after a restore.
#include "snapshot.h"
#include <string.h>

// Rejoin position as the save stores it (u32 tile, tile-local x y z, yaw): GameMan keeps the oldest of the
// last five grounded positions; before the first one, the player's own tile-local position is used.
typedef void *(*ChrGet_t)(void *chr, void *out);
void take_position(uint8_t *base)
{
    uint8_t *gm = *(uint8_t **)(base + GAME_MAN_RVA), *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA), out[20];
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

// Level and runes (PlayerGameData +0x68 level, +0x6c held runes, +0x110 runes gained). The save can be older
// than the snapshot, so a restore gives back runes earned after the game last wrote it.
void take_player(uint8_t *base)
{
    uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA), *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    if (!pgd)
        return;
    int32_t v[3] = {*(int32_t *)(pgd + 0x68), *(int32_t *)(pgd + 0x6c), *(int32_t *)(pgd + 0x110)};
    snap_write(SNAP_PLAYER, (uint8_t *)v, sizeof v);
    log_line(LOG_INFO, "player level=%d runes=%d gained=%d", v[0], v[1], v[2]);
}

void restore_player(uint8_t *base, const int32_t v[3])
{
    uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA), *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    if (!pgd)
        return;
    int32_t *lv = (int32_t *)(pgd + 0x68), *runes = (int32_t *)(pgd + 0x6c), *gained = (int32_t *)(pgd + 0x110);
    log_line(LOG_INFO, "player-restore level=%d/%d runes=%d/%d gained=%d/%d", *lv, v[0], *runes, v[1], *gained, v[2]);
    if (*lv == v[0] && v[2] > *gained)
        *runes += v[2] - *gained, *gained = v[2];
}

// A restored player keeps the session player number 2 it got from the fake host (player 1), and only sees
// ground items targeted at its own number. The game's leave path resets it with FUN_14066f2d0(PlayerIns, 1).
typedef void (*SetPlayerNo_t)(void *player, int no);
typedef char (*ItemVisible_t)(void *rec);

void become_player_one(uint8_t *base, uint8_t *session)
{
    uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA), *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA), *player = wcm ? *(uint8_t **)(wcm + 0x174e8) : NULL;
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
