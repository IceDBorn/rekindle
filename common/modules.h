#pragma once
#include <stdint.h>

// Calls between the restore side (rekindle/) and the snapshot side (snapshot/) of rekindle.dll.

// run_marker.c: multi_game_id of the run in progress, 0 outside a run.
extern uint64_t run_mgid;
// run_marker.c: the run is over; drops the marker and the mate list.
void clear_marker(const char *why);

// snapshot/main.c: reads the settings and waits for Steam networking on its own thread.
void snapshot_start(void);
// snapshot/clock.c: the run being restored and its snapshot clock, set by the auto restore.
void snapshot_restoring(uint64_t mgid, uint64_t clock);
// rekindle/relay.c: the restored world has loaded, so a teammate who restores later can join (nrk_ready=1).
void relay_ready(void);
