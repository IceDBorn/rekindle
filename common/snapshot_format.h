#pragma once

// Files next to rekindle.dll.
#define INI_FILE "rekindle.ini"
#define RUN_FILE "rekindle_run_%llu.bin"
#define RUN_TMP_FILE "rekindle_run.tmp"
#define ACTIVE_RUN_FILE "rekindle_active_run.txt"
#define MATES_FILE "rekindle_mates.txt"

// Snapshot records are u32 ms, u8 type, u32 len, data. Type 0 is a marker (len 5) or the clock (len 8 + 0x200);
// game messages keep their own type, and Rekindle's own records use these.
#define SNAP_MAP_ITEM 0xf1
#define SNAP_POSITION 0xf2
#define SNAP_PLAYER 0xf3
#define SNAP_ENEMIES 0xf4
