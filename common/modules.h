#pragma once
#include <stdint.h>

// Calls between the restore side (rekindle/) and the snapshot side (snapshot/) of rekindle.dll.

// run_marker.c: multi_game_id of the run in progress, 0 outside a run.
extern uint64_t run_mgid;
// fake_host.c: the player's own Steam id, from the save folder.
extern uint64_t fake_self;
// run_marker.c: the run is over; drops the marker and the mate list.
void clear_marker(const char *why);
// run_marker.c: a restore ended in the Roundtable; the marker is dropped only after the third time in a row.
void marker_rejoin_failed(void);
// run_marker.c: a restore got back into the run, so the failed rejoin count starts over.
void marker_rejoined(void);

// snapshot/main.c: reads the settings and waits for Steam networking on its own thread.
void snapshot_start(void);
// snapshot/clock.c: the run being restored and its snapshot clock, set by the auto restore.
void snapshot_restoring(uint64_t mgid, uint64_t clock);
// snapshot/snapshot.c: the game reported a boss kill; a day 2 boss kill takes a snapshot despite the boss phase.
void snapshot_boss_killed(void);
// snapshot/snapshot.c: a restore failed, so the next one uses the snapshot from before the day 2 boss died, if any.
void snapshot_use_pre_day3(uint64_t mgid);
// snapshot/player.c: a player's number recorded for the run, 0 if unknown. Usable before the run starts.
int run_player_number(uint64_t mgid, uint64_t id);
// rekindle/relay.c: the restored world has loaded, so a teammate who restores later can join (nrk_ready=1).
void relay_ready(void);
// rekindle/relay.c: the restore ended in the Roundtable, so the relay lobby is left on the next tick.
void relay_abandon(void);
