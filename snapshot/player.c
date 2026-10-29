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

uint32_t player_tile(uint8_t *base)
{
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA), *pl = wcm ? *(uint8_t **)(wcm + 0x174e8) : NULL;
    uint32_t tile = 0xffffffff;
    if (pl && *(void **)(pl + 0x5f0))
        ((ChrGet_t)(base + 0x66b700))(pl, &tile);
    return tile;
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

// A restored player gets its session player number from the fake host, and only sees ground items targeted at
// its own number. The run's number is in the player list (NetMan +0x68, list at +0xa8, see hook_player_list);
// two players on one number end the run when either leaves. FUN_14066f2d0(PlayerIns, no) sets it.
typedef void (*SetPlayerNo_t)(void *player, int no);
typedef char (*ItemVisible_t)(void *rec);

#pragma pack(push, 1)
struct member_no { uint64_t id; int32_t no; };
#pragma pack(pop)
static struct member_no run_nos[8];
static int run_no_count;
static uint64_t run_nos_mgid;

static bool restored_run(void)
{
    return run_nos_mgid && run_nos_mgid == run_mgid;
}

int recorded_number(uint64_t id)
{
    for (int i = 0; restored_run() && i < run_no_count; i++)
        if (run_nos[i].id == id)
            return run_nos[i].no;
    return 0;
}

// A player missing from the record keeps its number unless a recorded player holds it; two players on one
// number end the run when either leaves.
int run_number_for(uint64_t id, int current)
{
    int no = recorded_number(id);
    if (no || !restored_run())
        return no ? no : current;
    for (int i = 0; i < run_no_count; i++)
        if (run_nos[i].no == current) {
            for (no = 1;; no++) {
                int k = 0;
                while (k < run_no_count && run_nos[k].no != no)
                    k++;
                if (k == run_no_count)
                    return no;
            }
        }
    return current;
}

// Session members (SessionMan +0x78, stride 0xf0, id +0x10, number +0xd4). In a restored run the record stays the
// truth, so a snapshot taken while a teammate is away keeps their number.
void take_numbers(uint8_t *base)
{
    uint8_t *ses = *(uint8_t **)(base + SESSION_MAN_RVA);
    struct member_no out[8];
    int n = 0;
    for (uint8_t *m = ses ? *(uint8_t **)(ses + 0x78) : NULL, *end = ses ? *(uint8_t **)(ses + 0x80) : NULL;
         m && m < end && n < 8; m += 0xf0) {
        uint64_t id = *(uint64_t *)(m + 0x10);
        int cur = *(int *)(m + 0xd4);
        if (cur <= 0)
            continue;
        log_line(LOG_DEBUG, "number %llx=%d self=%d", (unsigned long long)id, cur, m[0xe1]);
        if (!restored_run())
            out[n++] = (struct member_no){id, cur};
        else if (!recorded_number(id) && run_no_count < 8)
            run_nos[run_no_count] = (struct member_no){id, run_number_for(id, cur)}, run_no_count++;
    }
    if (restored_run())
        memcpy(out, run_nos, run_no_count * sizeof *out), n = run_no_count;
    if (n)
        snap_write(SNAP_NUMBERS, (uint8_t *)out, n * (int)sizeof *out);
}

void queue_numbers(uint64_t mgid, const uint8_t *d, uint32_t l)
{
    if (l % sizeof(struct member_no))
        return;
    run_nos_mgid = mgid;
    run_no_count = (int)(l / sizeof(struct member_no)) < 8 ? (int)(l / sizeof(struct member_no)) : 8;
    memcpy(run_nos, d, run_no_count * sizeof *run_nos);
    for (int i = 0; i < run_no_count; i++)
        log_line(LOG_INFO, "numbers-restore %llx=%d", (unsigned long long)run_nos[i].id, run_nos[i].no);
}

int run_player_number(uint64_t mgid, uint64_t id)
{
    if (run_nos_mgid != mgid)
        load_numbers(mgid);
    for (int i = 0; run_nos_mgid == mgid && i < run_no_count; i++)
        if (run_nos[i].id == id)
            return run_nos[i].no;
    return 0;
}

// The session can be empty while the rejoin request is built, so the save folder's id is used.
int own_recorded_number(void)
{
    return recorded_number(fake_self);
}

static int run_number(uint8_t *base, uint64_t self)
{
    if (recorded_number(self))
        return recorded_number(self);
    uint8_t *net = *(uint8_t **)(base + NET_MAN_RVA), *obj = net ? *(uint8_t **)(net + 0x68) : NULL;
    uint8_t *l = obj ? *(uint8_t **)(obj + 0xa8) : NULL;
    for (uint8_t *e = l ? *(uint8_t **)(l + 0x70) : NULL; e && e < *(uint8_t **)(l + 0x78); e += 0x20)
        if (*(uint64_t *)e == self)
            return *(int *)(e + 0x10);
    return 0;
}

void restore_player_number(uint8_t *base, uint8_t *session)
{
    uint8_t *gdm = *(uint8_t **)(base + GAME_DATA_MAN_RVA), *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    uint8_t *wcm = *(uint8_t **)(base + WORLD_CHR_MAN_RVA), *player = wcm ? *(uint8_t **)(wcm + 0x174e8) : NULL;
    int was = pgd ? *(int *)(pgd + 8) : -1, no = 0;
    uint8_t *self = NULL;
    for (uint8_t *m = *(uint8_t **)(session + 0x78), *end = *(uint8_t **)(session + 0x80); m && m < end; m += 0xf0)
        if (m[0xe1])
            self = m;
    if (self)
        no = run_number(base, *(uint64_t *)(self + 0x10));
    if (!no) {
        log_line(LOG_WARN, "player-number was=%d, not in the player list, kept", was);
        return;
    }
    if (pgd)
        *(int *)(pgd + 0xec) = no;
    if (!pgd || !player || was == no)
        return;
    ((SetPlayerNo_t)(base + 0x66f2d0))(player, no);
    // The self member entry keeps its number at +0xd4 for the number allocator.
    *(int *)(self + 0xd4) = no;
    // Items already on the ground got no visual; +0xf8 makes the manager update retry it.
    uint8_t *man = *(uint8_t **)(base + MAP_ITEM_MAN_RVA);
    int shown = 0;
    for (int i = 0; man && i < 5; i++)
        for (uint8_t *n = *(uint8_t **)(man + item_heads[i]); n; n = *(uint8_t **)n)
            if (((ItemVisible_t)(base + 0x572c10))(n + 0x20))
                n[0xf8] = 1, shown++;
    log_line(LOG_INFO, "player-number was=%d now=%d retried=%d", was, *(int *)(pgd + 8), shown);
}
