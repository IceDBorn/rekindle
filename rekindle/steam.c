// Steam: wraps the interfaces the game gets (matchmaking, friends, client, utils, user, networking) so calls
// can be logged and the fakes installed, and wraps callback objects the same way.
#include "rekindle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Original vtables per wrapped interface; the asm handlers index them by key >> 16.
void **steam_orig_tables[16];
static struct {
    const char *name;
    void **orig, **wrapped;
} ifaces[16];
static int iface_count;

static const char *mm_names[] = {
    "GetFavoriteGameCount", "GetFavoriteGame", "AddFavoriteGame", "RemoveFavoriteGame", "RequestLobbyList",
    "AddRequestLobbyListStringFilter", "AddRequestLobbyListNumericalFilter", "AddRequestLobbyListNearValueFilter",
    "AddRequestLobbyListFilterSlotsAvailable", "AddRequestLobbyListDistanceFilter",
    "AddRequestLobbyListResultCountFilter", "AddRequestLobbyListCompatibleMembersFilter", "GetLobbyByIndex",
    "CreateLobby", "JoinLobby", "LeaveLobby", "InviteUserToLobby", "GetNumLobbyMembers", "GetLobbyMemberByIndex",
    "GetLobbyData", "SetLobbyData", "GetLobbyDataCount", "GetLobbyDataByIndex", "DeleteLobbyData",
    "GetLobbyMemberData", "SetLobbyMemberData", "SendLobbyChatMsg", "GetLobbyChatEntry", "RequestLobbyData",
    "SetLobbyGameServer", "GetLobbyGameServer", "SetLobbyMemberLimit", "GetLobbyMemberLimit", "SetLobbyType",
    "SetLobbyJoinable", "GetLobbyOwner", "SetLobbyOwner", "SetLinkedLobby",
};

void steam_log_call(uint64_t key, uint64_t *args, uint64_t ret, int post);

// Post handler calls the original and logs the result; pre handler logs, then tail-jumps (for stack-arg methods).
__asm__(".globl steam_thunk_post\n"
        "steam_thunk_post:\n"
        "  push %rbx\n"
        "  sub $0x50, %rsp\n"
        "  mov %r10, %rbx\n"
        "  mov %rcx, 0x20(%rsp)\n"
        "  mov %rdx, 0x28(%rsp)\n"
        "  mov %r8, 0x30(%rsp)\n"
        "  mov %r9, 0x38(%rsp)\n"
        "  mov %rbx, %rax\n"
        "  shr $16, %rax\n"
        "  lea steam_orig_tables(%rip), %r11\n"
        "  mov (%r11,%rax,8), %r11\n"
        "  movzwl %bx, %eax\n"
        "  call *(%r11,%rax,8)\n"
        "  mov %rax, 0x40(%rsp)\n"
        "  movq %xmm0, 0x48(%rsp)\n"
        "  mov %rbx, %rcx\n"
        "  lea 0x20(%rsp), %rdx\n"
        "  mov %rax, %r8\n"
        "  mov $1, %r9d\n"
        "  call steam_log_call\n"
        "  mov 0x40(%rsp), %rax\n"
        "  movq 0x48(%rsp), %xmm0\n"
        "  add $0x50, %rsp\n"
        "  pop %rbx\n"
        "  ret\n"
        ".globl steam_thunk_pre\n"
        "steam_thunk_pre:\n"
        "  sub $0x68, %rsp\n"
        "  mov %rcx, 0x20(%rsp)\n"
        "  mov %rdx, 0x28(%rsp)\n"
        "  mov %r8, 0x30(%rsp)\n"
        "  mov %r9, 0x38(%rsp)\n"
        "  movq %xmm0, 0x40(%rsp)\n"
        "  movq %xmm1, 0x48(%rsp)\n"
        "  movq %xmm2, 0x50(%rsp)\n"
        "  movq %xmm3, 0x58(%rsp)\n"
        "  mov %r10, 0x60(%rsp)\n"
        "  mov %r10, %rcx\n"
        "  lea 0x20(%rsp), %rdx\n"
        "  xor %r8d, %r8d\n"
        "  xor %r9d, %r9d\n"
        "  call steam_log_call\n"
        "  mov 0x20(%rsp), %rcx\n"
        "  mov 0x28(%rsp), %rdx\n"
        "  mov 0x30(%rsp), %r8\n"
        "  mov 0x38(%rsp), %r9\n"
        "  movq 0x40(%rsp), %xmm0\n"
        "  movq 0x48(%rsp), %xmm1\n"
        "  movq 0x50(%rsp), %xmm2\n"
        "  movq 0x58(%rsp), %xmm3\n"
        "  mov 0x60(%rsp), %r10\n"
        "  add $0x68, %rsp\n"
        "  mov %r10, %rax\n"
        "  shr $16, %rax\n"
        "  lea steam_orig_tables(%rip), %r11\n"
        "  mov (%r11,%rax,8), %r11\n"
        "  movzwl %r10w, %eax\n"
        "  jmp *(%r11,%rax,8)\n");
void steam_thunk_post(void);
void steam_thunk_pre(void);

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION m;
    if ((uintptr_t)p < 0x10000 || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(m.Protect & 0xee))
        return 0;
    return (uint8_t *)p + n <= (uint8_t *)m.BaseAddress + m.RegionSize;
}

// Prints a pointer as a quoted string when it points at printable text, else as hex.
static int fmt_arg(char *dst, size_t cap, uint64_t v)
{
    const char *s = (const char *)(uintptr_t)v;
    MEMORY_BASIC_INFORMATION m;
    if (readable(s, 1) && VirtualQuery(s, &m, sizeof m)) {
        size_t room = (size_t)((const char *)m.BaseAddress + m.RegionSize - s), n = 0;
        while (n < 120 && n < room && s[n] >= 0x20 && s[n] < 0x7f)
            n++;
        if (n && (n >= 120 || (n < room && s[n] == 0)))
            return snprintf(dst, cap, "\"%.*s\"", (int)n, s);
    }
    return snprintf(dst, cap, "0x%llx", (unsigned long long)v);
}


static void wrap_iface(void *obj, const char *ver);

void steam_log_call(uint64_t key, uint64_t *args, uint64_t ret, int post)
{
    int k = (int)(key >> 16), slot = (int)(key & 0xffff);
    const char *name = ifaces[k].name;
    // The client only matters for the interfaces it hands out.
    if (!strncmp(name, "SteamClient", 11)) {
        // GetISteamUtils takes (pipe, version); the other getters take (user, pipe, version).
        uint64_t ver = slot == 9 ? args[2] : args[3];
        if (post && (slot == 5 || slot == 8 || slot == 9 || slot == 10 || slot == 12 || slot == 16) &&
            readable((void *)(uintptr_t)ver, 1))
            wrap_iface((void *)(uintptr_t)ret, (const char *)(uintptr_t)ver);
        return;
    }
    if (log_level < LOG_DEBUG)
        return;
    int mm = !strncmp(name, "SteamMatchMaking", 16);
    char line[1200], a[3][300];
    for (int i = 0; i < 3; i++)
        fmt_arg(a[i], sizeof a[i], args[i + 1]);
    int n = snprintf(line, sizeof line, "%s.%s(%s, %s, %s)", name,
                     mm && slot < 38 ? mm_names[slot] : "", a[0], a[1], a[2]);
    if (!(mm && slot < 38))
        n = snprintf(line, sizeof line, "%s.slot%d(%s, %s, %s)", name, slot, a[0], a[1], a[2]);
    if (post) {
        char r[300];
        // CSteamID returns go through a hidden pointer, so rax points at the id.
        if (mm && (slot == 12 || slot == 18 || slot == 35) && readable((void *)(uintptr_t)ret, 8))
            snprintf(r, sizeof r, "id=%llu", *(unsigned long long *)(uintptr_t)ret);
        else if (mm && (slot == 19 || slot == 24))
            fmt_arg(r, sizeof r, ret);
        else
            snprintf(r, sizeof r, "0x%llx", (unsigned long long)ret);
        snprintf(line + n, sizeof line - n, " -> %s", r);
    }
    log_dedup("Steam", line);
}

static void wrap_iface(void *obj, const char *ver)
{
    int kind = !strncmp(ver, "SteamMatchMaking", 16) ? 1 : !strncmp(ver, "SteamFriends", 12) ? 2
             : !strncmp(ver, "SteamClient", 11)      ? 3 : !strncmp(ver, "SteamUtils", 10) ? 4
             : !strncmp(ver, "SteamUser", 9) && strncmp(ver, "SteamUserStats", 14) ? 5
             : !strncmp(ver, "SteamNetworking0", 16) ? 6 : 0;
    if (!kind || !readable(obj, 8))
        return;
    EnterCriticalSection(&log_lock);
    void **vt = *(void ***)obj;
    int k;
    for (k = 0; k < iface_count; k++)
        if (ifaces[k].wrapped == vt || ifaces[k].orig == vt)
            break;
    if (k == iface_count && k < 16) {
        int slots = kind == 1 ? 44 : kind == 2 ? 100 : 40;
        while (slots && !readable(vt, slots * 8))
            slots--;
        void **nv = VirtualAlloc(NULL, slots * 8 + slots * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        uint8_t *code = (uint8_t *)(nv + slots);
        for (int s = 0; s < slots; s++) {
            // Matchmaking methods with stack arguments, and every friends method, log before the call only.
            int pre = kind == 2 || kind >= 4 ||
                      (kind == 1 && (s == 1 || s == 2 || s == 3 || s == 22 || s == 27 || s == 29 || s == 30)) ||
                      (kind == 3 && s != 5 && s != 8 && s != 9 && s != 10 && s != 12 && s != 16);
            uint8_t *t = code + s * 32;
            uint64_t key = ((uint64_t)k << 16) | s, h = (uint64_t)(pre ? steam_thunk_pre : steam_thunk_post);
            t[0] = 0x49, t[1] = 0xba, memcpy(t + 2, &key, 8);
            t[10] = 0x49, t[11] = 0xbb, memcpy(t + 12, &h, 8);
            t[20] = 0x41, t[21] = 0xff, t[22] = 0xe3;
            nv[s] = t;
        }
        install_fakes(kind, vt, nv, slots);
        if (kind == 1)
            mm_self = obj;
        else if (kind == 4)
            utils_self = obj;
        ifaces[k].name = _strdup(ver);
        ifaces[k].orig = vt;
        ifaces[k].wrapped = nv;
        steam_orig_tables[k] = vt;
        iface_count++;
        char info[160];
        snprintf(info, sizeof info, "%s obj=%p vtable=%p slots=%d", ver, obj, (void *)vt, slots);
        LeaveCriticalSection(&log_lock);
        log_event("SteamWrap", NULL, info, NULL, 0);
        EnterCriticalSection(&log_lock);
    }
    if (k < iface_count && vt == ifaces[k].orig)
        *(void ***)obj = ifaces[k].wrapped;
    LeaveCriticalSection(&log_lock);
}

typedef void *(*FindIface_t)(int32_t, const char *);
typedef void *(*CreateIface_t)(const char *);
typedef void (*RegCb_t)(void *, int);
typedef void (*UnregCb_t)(void *);
typedef void (*RegCr_t)(void *, uint64_t);
static FindIface_t real_find_iface;
static CreateIface_t real_create_iface;
static RegCb_t real_reg_cb;
static UnregCb_t real_unreg_cb;
static RegCr_t real_reg_cr, real_unreg_cr;

static void *hook_find_iface(int32_t user, const char *ver)
{
    void *p = real_find_iface(user, ver);
    wrap_iface(p, ver ? ver : "");
    return p;
}

static void *hook_create_iface(const char *ver)
{
    void *p = real_create_iface(ver);
    wrap_iface(p, ver ? ver : "");
    return p;
}

// Callback objects get a private vtable copy whose Run slots log; slot -1 holds the original vtable.
static void log_cb(void *self, int slot, void *param, uint64_t b, uint64_t c)
{
    if (log_level < LOG_DEBUG)
        return;
    void **orig = (*(void ***)self)[-1];
    int id = *(int *)((uint8_t *)self + 0xc), size = ((int (*)(void *))orig[2])(self);
    char line[1400];
    int n = snprintf(line, sizeof line, "cb=%p id=%d run%d size=%d b=%llu c=0x%llx data=", self, id, slot, size,
                     (unsigned long long)(b & 0xff), (unsigned long long)c);
    for (int i = 0; i < size && i < 256 && readable((uint8_t *)param + i, 1); i++)
        n += snprintf(line + n, sizeof line - n, "%02x", ((uint8_t *)param)[i]);
    log_dedup("SteamCallback", line);
}
// Lobby callbacks (504 to 516) show the real lobby as the game's id; 505 holds it twice.
static void cb_relay(void *self, void *p)
{
    int id = *(int *)((uint8_t *)self + 0xc);
    if (!real_lobby || id < 504 || id > 516)
        return;
    int size = ((int (*)(void *))((void **)(*(void ***)self)[-1])[2])(self);
    for (int o = 0; o + 8 <= size; o += 8)
        if (*(uint64_t *)((uint8_t *)p + o) == real_lobby)
            *(uint64_t *)((uint8_t *)p + o) = game_lobby;
}
static void cb_run0(void *self, void *p, uint64_t b, uint64_t c)
{
    cb_relay(self, p);
    log_cb(self, 0, p, b, c);
    ((Run_t)((void **)(*(void ***)self)[-1])[0])(self, p, b, c);
}
static void cb_run1(void *self, void *p, uint64_t b, uint64_t c)
{
    // LobbyDataUpdate_t (505): lobby u64, member u64, success byte at 16.
    if (fake_lobby && *(int *)((uint8_t *)self + 0xc) == 505 && *(uint64_t *)p == fake_lobby &&
        !((uint8_t *)p)[16]) {
        ((uint8_t *)p)[16] = 1;
        note(LOG_DEBUG, "505 success forced");
    }
    cb_relay(self, p);
    if (*(int *)((uint8_t *)self + 0xc) == 506 && ((uint32_t *)p)[6] & 1)
        note_mate(((uint64_t *)p)[1]);
    log_cb(self, 1, p, b, c);
    ((Run_t)((void **)(*(void ***)self)[-1])[1])(self, p, b, c);
}

static void wrap_cb(void *cb)
{
    static void **orig[256], **wrapped[256];
    static int count;
    EnterCriticalSection(&log_lock);
    void **vt = *(void ***)cb;
    int i;
    for (i = 0; i < count; i++)
        if (orig[i] == vt || wrapped[i] == vt)
            break;
    if (i == count && count < 256 && readable(vt, 8 * 8)) {
        void **nv = malloc(9 * 8);
        nv[0] = orig[count] = vt;
        memcpy(nv + 1, vt, 8 * 8);
        nv[1] = cb_run0;
        nv[2] = cb_run1;
        wrapped[count++] = nv + 1;
    }
    if (i < count && orig[i] == vt)
        *(void ***)cb = wrapped[i];
    LeaveCriticalSection(&log_lock);
}

static void hook_reg_cb(void *cb, int id)
{
    char info[64];
    snprintf(info, sizeof info, "cb=%p id=%d", cb, id);
    log_event("RegisterCallback", NULL, info, NULL, 0);
    if (id == 143)
        cb143 = cb;
    else if (id == 506)
        cb506 = cb;
    wrap_cb(cb);
    real_reg_cb(cb, id);
}

static void hook_unreg_cb(void *cb)
{
    char info[64];
    snprintf(info, sizeof info, "cb=%p id=%d", cb, *(int *)((uint8_t *)cb + 0xc));
    log_dedup("UnregisterCallback", info);
    real_unreg_cb(cb);
}

static void hook_reg_cr(void *cb, uint64_t call)
{
    char info[96];
    snprintf(info, sizeof info, "cb=%p id=%d call=0x%llx", cb, *(int *)((uint8_t *)cb + 0xc),
             (unsigned long long)call);
    log_event("RegisterCallResult", NULL, info, NULL, 0);
    wrap_cb(cb);
    real_reg_cr(cb, call);
}

static void hook_unreg_cr(void *cb, uint64_t call)
{
    char info[96];
    snprintf(info, sizeof info, "cb=%p call=0x%llx", cb, (unsigned long long)call);
    log_event("UnregisterCallResult", NULL, info, NULL, 0);
    real_unreg_cr(cb, call);
}

typedef void (*RunCbs_t)(void);
static RunCbs_t real_run_cbs;

static void hook_run_cbs(void)
{
    real_run_cbs();
    emu_callbacks();
    relay_tick();
}

const struct import_hook steam_imports[] = {
    {"SteamInternal_FindOrCreateUserInterface", hook_find_iface, (void **)&real_find_iface},
    {"SteamInternal_CreateInterface", hook_create_iface, (void **)&real_create_iface},
    {"SteamAPI_RegisterCallback", hook_reg_cb, (void **)&real_reg_cb},
    {"SteamAPI_RunCallbacks", hook_run_cbs, (void **)&real_run_cbs},
    {"SteamAPI_UnregisterCallback", hook_unreg_cb, (void **)&real_unreg_cb},
    {"SteamAPI_RegisterCallResult", hook_reg_cr, (void **)&real_reg_cr},
    {"SteamAPI_UnregisterCallResult", hook_unreg_cr, (void **)&real_unreg_cr},
};
const int steam_import_count = sizeof steam_imports / sizeof *steam_imports;
