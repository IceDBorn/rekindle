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
    // PROBE: keep the snapshot this restore used; the restored game overwrites the run's file.
    char kept[MAX_PATH + 8];
    snprintf(kept, sizeof kept, "%s.used", path);
    CopyFileA(path, kept, FALSE);
    mod_path(path, MATES_FILE);
    unsigned long long mm = 0, m0 = 0, m1 = 0, lobby = 0;
    if ((f = fopen(path, "r"))) {
        int k = fscanf(f, "%llu %llu %llu", &mm, &m0, &m1);
        if (k >= 2 && mm == mgid) {
            run_mates[0] = m0, run_mates[1] = k == 3 ? m1 : 0;
            fake_host = m0;
            note(LOG_INFO, "Auto restore fake host = mate %llu", m0);
            // Without a rejoin record in the save (a quit), plant the run's own lobby and check it is still alive.
            if (fscanf(f, " lobby %llu", &lobby) == 1 && lobby)
                fake_lobby = run_lobby = lobby, lobby_from_save = true;
            unsigned ps = 0, p0 = 0, p1 = 0;
            if (fscanf(f, " pids %u %u %u", &ps, &p0, &p1) >= 1) {
                self_pid = ps, run_pids[0] = p0, run_pids[1] = p1;
                note(LOG_INFO, "Auto restore player ids self=%u mates=%u %u", ps, p0, p1);
            }
        }
        fclose(f);
    }
    uint64_t clock;
    uint8_t box[20];
    bool have_box = build_answers(mgid, snapshot, n, &clock, box);
    free(snapshot);
    snprintf(save, MAX_PATH, "%s\\Nightreign\\%llu\\NR0000.sl2", appdata ? appdata : "", (unsigned long long)fake_self);
    int r = plant_save(save, mgid, box, have_box);
    note(r < 0 ? LOG_ERROR : LOG_INFO, "Auto restore mgid=%llu plant=%d box=%d", mgid, r, have_box);
    if (r < 0) {
        // A save that cannot take the run makes the marker stale; its fake host and lobby would block joins.
        fake_lobby = 0, fake_host = 0, lobby_from_save = false, self_pid = 0;
        clear_marker("restore failed");
        return 0;
    }
    restore_mgid = mgid;
    if (clock)
        snapshot_restoring(mgid, clock);
    return 0;
}
