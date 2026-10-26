#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "game.h"
#include "log.h"
#include "snapshot_format.h"
#include "util.h"

typedef void *(*Ctor_t)(void *obj);

// main.c: snapshot timing and restore switches from rekindle.ini.
extern uint64_t first_snapshot_ms, snapshot_every_ms, resume_snapshots_ms;
extern bool restore_items, restore_runes, boss_drops, graces, enemy_hp;

// snapshot.c: what the game sends the fake peer while a snapshot is taken goes to the run file.
extern uint64_t fake_peer, snap_mgid, done_mgid;
// Set while rekindle.dll's run marker names a run that has no finished snapshot yet.
extern volatile LONG run_request;
extern __thread bool in_join_build;
void snap_write(uint32_t type, const uint8_t *data, int len);
void snap_mark(uint8_t kind, uint32_t id);
bool snapshot_install(void);

// join.c, clock.c
void take_join(uint8_t *base);
extern uint64_t restore_mgid, restore_clock;
void snap_clock(uint8_t *base);
void read_restore_clock(void);
void apply_restore_clock(void);
void restore_day(uint8_t *base, int day);

// map_items.c, player.c, graces.c, enemies.c
extern const int item_heads[5];
void take_map_items(uint8_t *base);
int restore_map_item(uint8_t *base, uint8_t *man, uint8_t *d, uint32_t l);
void take_position(uint8_t *base);
void take_player(uint8_t *base);
void restore_player(uint8_t *base, const int32_t v[3]);
void become_player_one(uint8_t *base, uint8_t *session);
void fix_graces(uint8_t *base);
void take_enemies(uint8_t *base);
void queue_enemy_hp(const uint8_t *d, uint32_t l);
void restore_enemies(uint8_t *base, uint64_t now);

// restore.c
void restore_snapshot(uint64_t mgid);
