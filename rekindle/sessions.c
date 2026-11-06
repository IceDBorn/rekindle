// Steam sessions: keeps a SteamNetworkingMessages session open to every teammate, so mods that read ping and
// loss from that API (made for Seamless Co-op) work in the vanilla game too, and the labels show its ping.
// The game's own traffic is untouched.
#include "rekindle.h"
#include <string.h>

#define SESSION_CHANNEL 0x524b
#define KEEPALIVE_MS 1000
// k_nSteamNetworkingSend_NoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession
#define SEND_FLAGS (1 | 32)

struct identity {
    int32_t type, size;
    union {
        uint64_t steam_id;
        char raw[128];
    };
};

typedef int (*SendToUser_t)(void *, const struct identity *, const void *, uint32_t, int, int);
typedef int (*Receive_t)(void *, int, void **, int);
typedef int (*SessionInfo_t)(void *, const struct identity *, void *, void *);
typedef void *(*FindIface_t)(int32_t, const char *);
typedef int32_t (*SteamUser_t)(void);
typedef void (*Release_t)(void *);

static SessionInfo_t real_session_info;
static volatile LONG64 other_read_ms;

// Another mod that reads session ping shows ping itself. Rekindle reads through real_session_info, so every
// call that lands here comes from someone else.
static int hook_session_info(void *self, const struct identity *who, void *info, void *status)
{
    if (!InterlockedExchange64(&other_read_ms, (LONG64)GetTickCount64())) {
        char name[MAX_PATH] = "?";
        HMODULE m = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)__builtin_return_address(0), &m))
            GetModuleFileNameA(m, name, sizeof name);
        note(LOG_INFO, "Steam session ping read by %s, labels leave the ping to it", name);
    }
    return real_session_info(self, who, info, status);
}

bool ping_shown_elsewhere(void)
{
    LONG64 t = other_read_ms;
    return t && GetTickCount64() - (uint64_t)t < 10000;
}

static void *find_messages(HMODULE api)
{
    FindIface_t find = (FindIface_t)(void *)GetProcAddress(api, "SteamInternal_FindOrCreateUserInterface");
    SteamUser_t user = (SteamUser_t)(void *)GetProcAddress(api, "SteamAPI_GetHSteamUser");
    int32_t h = find && user ? user() : 0;
    return h ? find(h, "SteamNetworkingMessages002") : NULL;
}

DWORD WINAPI sessions_thread(LPVOID unused)
{
    (void)unused;
    if (!cfg("ping", "steam_sessions", 1))
        return 0;
    HMODULE api = NULL;
    // Waits for the game's own Steam setup; SteamFriends is created right after SteamAPI_Init.
    for (int i = 0; i < 1200 && (!friends_self || !(api = GetModuleHandleA("steam_api64.dll"))); i++)
        Sleep(250);
    Release_t release = NULL;
    if (api)
        release = (Release_t)(void *)GetProcAddress(api, "SteamAPI_SteamNetworkingMessage_t_Release");
    void *msgs = api && release ? find_messages(api) : NULL;
    if (!msgs) {
        note(LOG_WARN, "Steam sessions off: no SteamNetworkingMessages002");
        return 0;
    }
    void **vt = *(void ***)msgs;
    // Every caller shares this object, so its vtable slot sees other mods' calls too.
    real_session_info = (SessionInfo_t)vt[5];
    DWORD old;
    if (VirtualProtect(&vt[5], 8, PAGE_READWRITE, &old)) {
        vt[5] = (void *)hook_session_info;
        VirtualProtect(&vt[5], 8, old, &old);
    }
    note(LOG_INFO, "Steam sessions on");
    for (uint64_t ticks = 0;; ticks++, Sleep(KEEPALIVE_MS)) {
        uint64_t ids[8];
        int n = ping_peer_ids(ids, 8);
        for (int i = 0; i < n; i++) {
            struct identity who = {16, 8, {.steam_id = ids[i]}};
            // Sending also accepts the teammate's session to us, so no session request callback is needed.
            static const char keepalive[4] = "RKS";
            ((SendToUser_t)vt[0])(msgs, &who, keepalive, sizeof keepalive, SEND_FLAGS, SESSION_CHANNEL);
            int32_t status[64] = {0};
            int state = real_session_info(msgs, &who, NULL, status);
            // k_ESteamNetworkingConnectionState_Connected; status[1] is m_nPing.
            if (state == 3 && status[1] >= 0)
                ping_steam(ids[i], (uint32_t)status[1]);
            if (ticks % 10 == 0)
                note(LOG_DEBUG, "Steam session %llx state=%d ping=%d", (unsigned long long)ids[i], state, status[1]);
        }
        void *in[32];
        for (int got; (got = ((Receive_t)vt[1])(msgs, SESSION_CHANNEL, in, 32)) > 0;)
            for (int k = 0; k < got; k++)
                release(in[k]);
    }
}
