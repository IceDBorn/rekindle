// Day clock: WorldAreaTime (StormTimer finds the same global) keeps a packed qword at +0x100,
// hour at bit 34, minute at 39, second at 45.
#include "snapshot.h"
#include <stdio.h>
#include <string.h>

uint64_t restore_mgid, restore_clock;

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

// rekindle.dll's auto restore names the restored run and its snapshot clock in rekindle_clock.txt.
void read_restore_clock(void)
{
    char rc_path[MAX_PATH];
    mod_path(rc_path, CLOCK_FILE);
    FILE *rc = fopen(rc_path, "r");
    if (rc) {
        unsigned long long m = 0, c = 0;
        if (fscanf(rc, "%llu %llx", &m, &c) == 2)
            restore_mgid = m, restore_clock = c;
        fclose(rc);
    }
}

void apply_restore_clock(void)
{
    // The join answer (kind 8) already set the snapshot clock; writing it again here would rewind the storm.
    uint8_t *w = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + WORLD_AREA_TIME_RVA);
    if (w)
        log_line(LOG_INFO, "restore-clock mgid=%llu snapshot=%llx now=%llx day=%d", (unsigned long long)restore_mgid,
                 (unsigned long long)restore_clock, (unsigned long long)*(uint64_t *)(w + 0x100), w[0x156]);
    restore_mgid = 0;
    char path[MAX_PATH];
    mod_path(path, CLOCK_FILE);
    DeleteFileA(path);
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
