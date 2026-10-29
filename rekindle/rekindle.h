#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "log.h"
#include "modules.h"
#include "util.h"

// An import of nightreign.exe that main.c points at hook, keeping the original in *real.
struct import_hook {
    const char *name;
    void *hook;
    void **real;
};
extern const struct import_hook http_imports[], steam_imports[];
extern const int http_import_count, steam_import_count;

// local_server.c: answers every WinHttpConnect when cl_server_redirector is absent.
extern volatile LONG local_port;
bool local_mode(void);

// lobby.c: Steam's answers for fake_lobby are rewritten so a rejoin to a dead lobby looks like a live one owned
// by fake_host.
#define DEFAULT_LOBBY 0x186000048928a92ULL
#define DEFAULT_HOST 76561200107749360ULL
extern uint64_t fake_lobby, fake_host, fake_self;
extern bool run_restored;
// The rejoin lobby came from the save; its first LobbyDataUpdate tells whether a teammate is still in the run.
extern bool lobby_from_save;
extern void **mm_orig, **utils_orig;
// After the replay the fake host leaves the lobby like a crashed host: 506 at host_gone_at, then self owns it.
extern uint64_t host_gone_at;
extern volatile LONG host_gone;
typedef void (*Run_t)(void *, void *, uint64_t, uint64_t);
typedef uint64_t *(*GetId_t)(void *, uint64_t *, uint64_t);
typedef bool (*Done_t)(void *, uint64_t, bool *);
typedef bool (*Result_t)(void *, uint64_t, void *, int, int, bool *);
void install_fakes(int kind, void **vt, void **nv, int slots);

// relay.c: lobby relay for a co-op rejoin after everyone crashed.
extern uint64_t game_lobby, real_lobby, restore_mgid;
extern void *mm_self, *utils_self;
uint64_t tr(uint64_t v);
void relay_wrap_mm(void **nv);
void relay_tick(void);
void relay_resolve(bool live);
// A 505 for the fake lobby that waits until the relay lobby is ready.
bool relay_hold_505(void);
bool emu_active(void);

// patches.c
extern bool keep_online_ok;
void keep_online(bool on);
DWORD WINAPI startup_patches(LPVOID unused);

// fake_host.c: answers the game's P2P link to fake_host.
extern CRITICAL_SECTION emu_lock;
extern uint64_t emu_mate2;
extern void *cb143, *cb506;
void emu_set_answers(uint8_t *intro, size_t intro_n, uint8_t *ans, size_t ans_n, uint8_t *str, size_t str_n);
void emu_wrap_user(void **vt, void **nv);
void emu_wrap_net(void **vt, void **nv);
void emu_callbacks(void);

static inline void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8, p[1] = (uint8_t)v; }
static inline void put_be32(uint8_t *p, uint32_t v) { put_be16(p, v >> 16), put_be16(p + 2, (uint16_t)v); }
static inline uint16_t get_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// ping.c: ping and packet loss next to teammate names.
extern void *friends_self, **friends_orig;
void ping_seen(void *self, void **vt, uint64_t id, const uint8_t *d, uint32_t n, int channel, bool incoming);
DWORD WINAPI ping_thread(LPVOID unused);

// run_marker.c: the run in progress and its lobby mates.
extern uint64_t run_mates[3], run_lobby;
extern uint32_t self_pid, run_pids[3];
uint32_t mate_pid(uint64_t id);
uint32_t peer_pid(uint64_t id);
void run_marker_http(const void *data, size_t n);
void note_mate(uint64_t id, uint64_t lobby);
void save_mates(void);
void reset_mates(void);
uint64_t trio_mate(void);

// answers.c, save.c, restore.c
bool build_answers(const uint8_t *s, size_t n, uint64_t *clock, uint8_t box[20]);
int plant_save(const char *path, uint64_t mgid, const uint8_t box[20], bool have_box);
uint64_t find_self(const char *appdata);
DWORD WINAPI auto_restore(LPVOID unused);

// eos.c, crash.c
int patch_eos(void);
LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep);
