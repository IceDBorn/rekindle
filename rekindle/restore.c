// Auto restore: on launch, a run marker with a snapshot plants the save and builds the fake host's answers.
#include "rekindle.h"
#include <stdio.h>
#include <stdlib.h>
#include "snapshot_format.h"

DWORD WINAPI auto_restore(LPVOID unused)
{
    (void)unused;
    char path[MAX_PATH], save[MAX_PATH];
    unsigned long long mgid = 0;
    // Mates are told apart from self, so this runs on every launch.
    const char *appdata = getenv("APPDATA");
    if (!fake_self)
        fake_self = find_self(appdata);
    // A rekindle_clock.txt left by a restore that never got far enough must not leak into this launch.
    mod_path(path, CLOCK_FILE);
    DeleteFileA(path);
    mod_path(path, ACTIVE_RUN_FILE);
    FILE *f = cfg("restore", "auto_restore", 1) ? fopen(path, "r") : NULL;
    if (!f)
        return 0;
    if (fscanf(f, "%llu", &mgid) != 1)
        mgid = 0;
    fclose(f);
    mod_path(path, RUN_FILE, mgid);
    size_t n = 0;
    uint8_t *snapshot = mgid && fake_lobby ? read_all(path, &n) : NULL;
    if (!snapshot) {
        note(LOG_ERROR, "Auto restore: no snapshot for mgid=%llu", mgid);
        return 0;
    }
    run_mgid = mgid;
    mod_path(path, MATES_FILE);
    unsigned long long mm = 0, m0 = 0, m1 = 0;
    if ((f = fopen(path, "r"))) {
        if (fscanf(f, "%llu %llu %llu", &mm, &m0, &m1) >= 2 && mm == mgid) {
            run_mates[0] = m0, run_mates[1] = m1;
            fake_host = m0;
            note(LOG_INFO, "Auto restore fake host = mate %llu", m0);
        }
        fclose(f);
    }
    uint64_t clock;
    uint8_t box[20];
    bool have_box = build_answers(snapshot, n, &clock, box);
    free(snapshot);
    snprintf(save, MAX_PATH, "%s\\Nightreign\\%llu\\NR0000.sl2", appdata ? appdata : "", (unsigned long long)fake_self);
    int r = plant_save(save, mgid, box, have_box);
    restore_mgid = mgid;
    note(r < 0 ? LOG_ERROR : LOG_INFO, "Auto restore mgid=%llu plant=%d box=%d", mgid, r, have_box);
    // rekindle_snapshot.dll reads rekindle_clock.txt once Steam is up, seconds after this thread has run.
    if (r >= 0 && clock) {
        mod_path(path, CLOCK_FILE);
        if ((f = fopen(path, "w")))
            fprintf(f, "%llu %llx\n", mgid, (unsigned long long)clock), fclose(f);
    }
    return 0;
}
