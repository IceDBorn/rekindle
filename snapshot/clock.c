// Day clock: WorldAreaTime (StormTimer finds the same global) keeps a packed qword at +0x100,
// hour at bit 34, minute at 39, second at 45.
#include "snapshot.h"
#include <string.h>

uint64_t restoring_mgid;
static uint64_t restore_clock;

// Clock record: type 0, len 8 + 0x200: the clock qword, then the WorldAreaTime struct for later study.
void snap_clock(uint8_t *base)
{
    uint8_t *w = *(uint8_t **)(base + WORLD_AREA_TIME_RVA);
    if (!w)
        return;
    uint8_t rec[8 + 0x200];
    memcpy(rec, w + 0x100, 8);
    memcpy(rec + 8, w, 0x200);
    snap_write(0, rec, sizeof rec);
}

void snapshot_restoring(uint64_t mgid, uint64_t clock)
{
    restore_clock = clock;
    restoring_mgid = mgid;
    load_numbers(mgid);
}

void apply_restore_clock(void)
{
    // The join answer (kind 8) already set the snapshot clock; writing it again here would rewind the storm.
    uint8_t *w = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + WORLD_AREA_TIME_RVA);
    if (w)
        log_line(LOG_INFO, "restore-clock mgid=%llu snapshot=%llx now=%llx day=%d", (unsigned long long)restoring_mgid,
                 (unsigned long long)restore_clock, (unsigned long long)*(uint64_t *)(w + 0x100), w[0x156]);
    restoring_mgid = 0;
}

// WorldPlayAreaManager keeps the day 1 and day 2 circle sets; a join loads day 1, so a day 2 run needs
// FUN_140691470(mgr, 1), which wants +0x2d set and [0x143c123f8] present.
typedef void (*LoadDay_t)(void *mgr, uint8_t day);
void restore_day(uint8_t *base, int day)
{
    uint8_t *w = *(uint8_t **)(base + WORLD_AREA_TIME_RVA);
    if (!w || day != 1 || w[0x156] == 1)
        return;
    bool ok = w[0x2d] && *(void **)(base + 0x3c123f8);
    if (ok)
        ((LoadDay_t)(base + 0x691470))(w, 1);
    log_line(ok ? LOG_INFO : LOG_WARN, "restore-day 2 loaded=%d", ok);
}

// The day clock stops at 23:59:59 once the boss phase starts (the timer is gone); a restore then starts before it.
bool boss_phase(uint8_t *base)
{
    uint8_t *w = *(uint8_t **)(base + WORLD_AREA_TIME_RVA);
    uint64_t q = w ? *(uint64_t *)(w + 0x100) : 0;
    return w && ((q >> 34) & 31) == 23 && ((q >> 39) & 63) == 59 && ((q >> 45) & 63) == 59;
}

// Real ms until the day clock stops, from its rate over the last 5 s or more (storm speed and skips included);
// -1 while the rate is unknown.
int64_t boss_eta_ms(uint8_t *base, uint64_t now)
{
    static uint64_t at_ms;
    static int at_clock = -1;
    static double rate;
    uint8_t *w = *(uint8_t **)(base + WORLD_AREA_TIME_RVA);
    if (!w)
        return -1;
    uint64_t q = *(uint64_t *)(w + 0x100);
    int c = (int)((q >> 34) & 31) * 3600 + (int)((q >> 39) & 63) * 60 + (int)((q >> 45) & 63);
    if (at_clock < 0 || c < at_clock)
        at_clock = c, at_ms = now, rate = 0;
    else if (now - at_ms >= 5000)
        rate = (double)(c - at_clock) / (double)(now - at_ms), at_clock = c, at_ms = now;
    return rate > 0 ? (int64_t)((23 * 3600 + 59 * 60 + 59 - c) / rate) : -1;
}

// PROBE: logs day, clock and map area changes, to find what day 3 looks like before and during its boss.
void world_probe(uint8_t *base)
{
    uint8_t *w = *(uint8_t **)(base + WORLD_AREA_TIME_RVA);
    static int last_day = -1, last_stop = -1;
    static uint32_t last_area = 0xffffffff;
    if (!w)
        return;
    uint64_t q = *(uint64_t *)(w + 0x100);
    uint32_t tile = player_tile(base), area = tile >> 24;
    int stop = boss_phase(base);
    if (w[0x156] == last_day && stop == last_stop && (area == last_area || tile == 0xffffffff))
        return;
    last_day = w[0x156], last_stop = stop, last_area = tile == 0xffffffff ? last_area : area;
    log_line(LOG_INFO, "probe world day=%d clock=%02d:%02d:%02d tile=%08x", w[0x156], (int)((q >> 34) & 31),
             (int)((q >> 39) & 63), (int)((q >> 45) & 63), tile);
}
