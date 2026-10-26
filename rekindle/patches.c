// Game patches: the AFK kick, the rejoin penalty and downed respawn at startup, and keep_online for restored runs.
#include "rekindle.h"
#include <string.h>
#include "patch.h"

// The AFK kick is a local idle timer in FUN_1403ba240 (warning at 270 s, kick at 300 s, from
// GameSystemCommonParam). Moving always counts as active, and the kick flag write stores 0.
DWORD WINAPI startup_patches(LPVOID unused)
{
    (void)unused;
    static const uint8_t move_old[] = {0x0f,0x2f,0x05,0xa2,0x02,0x5d,0x03,0x72,0x0e,0x33,0xff};
    static const uint8_t move_new[] = {0x0f,0x2f,0x05,0xa2,0x02,0x5d,0x03,0x90,0x90,0x33,0xff};
    static const uint8_t kick_old[] = {0x0f,0x2f,0xc8,0x76,0x19,0x66,0xc7,0x83,0xc4,0x01,0x00,0x00,0x01,0x01};
    static const uint8_t kick_new[] = {0x0f,0x2f,0xc8,0x76,0x19,0x66,0xc7,0x83,0xc4,0x01,0x00,0x00,0x00,0x00};
    // Rejoin penalty: CS::RejoinMapEnterStep calls FUN_140729200(mgr, skip=0), which takes a level, drops the
    // runes in a death box and sends the die log. skip=1 is what a penalty-free respawn passes.
    static const uint8_t pen1_old[] = {0x33,0xd2,0xe8,0x21,0x9f,0xae,0xff,0x83,0xbb,0xac,0x00,0x00,0x00,0xff};
    static const uint8_t pen1_new[] = {0xb2,0x01,0xe8,0x21,0x9f,0xae,0xff,0x83,0xbb,0xac,0x00,0x00,0x00,0xff};
    static const uint8_t pen2_old[] = {0x33,0xd2,0xe8,0x1b,0x9d,0xae,0xff,0x48,0x8b,0x05,0xbc,0xfb,0xfc,0x02};
    static const uint8_t pen2_new[] = {0xb2,0x01,0xe8,0x1b,0x9d,0xae,0xff,0x48,0x8b,0x05,0xbc,0xfb,0xfc,0x02};
    // A player who crashed while downed would come back downed; always take the respawn branch instead.
    static const uint8_t down_old[] = {0x84,0xc0,0x74,0x07,0x80,0x8b,0xc2,0x00,0x00,0x00,0x01,0x48,0x8b,0x3d};
    static const uint8_t down_new[] = {0x84,0xc0,0xeb,0x07,0x80,0x8b,0xc2,0x00,0x00,0x00,0x01,0x48,0x8b,0x3d};
    bool afk = cfg("penalties", "remove_afk_kick", 1), pen = cfg("penalties", "remove_rejoin_penalty", 1);
    bool m = !afk, k = !afk, p1 = !pen, p2 = !pen, d = !cfg("penalties", "respawn_if_downed", 1);
    for (int i = 0; i < 1200 && !(m && k && p1 && p2 && d); i++, Sleep(250)) {
        m = m || patch_bytes(0x1403ba2cf, move_old, move_new, sizeof move_old);
        k = k || patch_bytes(0x1403ba34a, kick_old, kick_new, sizeof kick_old);
        p1 = p1 || patch_bytes(0x140c3f2d8, pen1_old, pen1_new, sizeof pen1_old);
        p2 = p2 || patch_bytes(0x140c3f4de, pen2_old, pen2_new, sizeof pen2_old);
        d = d || patch_bytes(0x140c3fabc, down_old, down_new, sizeof down_old);
    }
    note(LOG_INFO, "Startup patches afk=%d/%d penalty=%d/%d downed=%d settings=%d%d%d", m, k, p1, p2, d, afk, pen,
         cfg("penalties", "respawn_if_downed", 1));
    if (!(m && k && p1 && p2 && d))
        note(LOG_WARN, "Startup patches not applied in time, the game bytes did not match");
    return 0;
}

// FUN_140b5da90 sets the "host lost" leave reason ([0x143c046a8]+0x13 = 1) that sends the game offline.
// During a restored run the fake host always leaves, so write 0 instead.
bool keep_online_ok;
// FUN_140df6890 sets +0x14 when self becomes lobby owner but the session can't take over; skip it too.

static bool patch_reason(uintptr_t at, const uint8_t code[10], bool on)
{
    uint8_t *p = (uint8_t *)at;
    DWORD old;
    if (memcmp(p, code, 10) || !VirtualProtect(p + 10, 1, PAGE_EXECUTE_READWRITE, &old))
        return false;
    p[10] = on ? 0 : 1;
    VirtualProtect(p + 10, 1, old, &old);
    return true;
}

void keep_online(bool on)
{
    static const uint8_t lost[] = {0x48, 0x8b, 0x05, 0x34, 0x68, 0x0a, 0x03, 0xc6, 0x40, 0x13};
    static const uint8_t owner[] = {0x48, 0x8b, 0x05, 0xc5, 0xdc, 0xe0, 0x02, 0xc6, 0x40, 0x14};
    if (!patch_reason(0x140b5de6d, lost, on)) {
        note(LOG_ERROR, "Keep online: patch bytes mismatch at 0x140b5de6d");
        return;
    }
    bool o = patch_reason(0x140df69dc, owner, on);
    if (!o)
        note(LOG_ERROR, "Keep online: patch bytes mismatch at 0x140df69dc");
    keep_online_ok = on;
    note(LOG_INFO, "Keep online %d owner=%d", on, o);
}
