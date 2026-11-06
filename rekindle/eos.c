// EOS stubs: unless a mod loaded the SDK, the game's EOS delay imports point here, so the SDK and EAC never load.
#include "rekindle.h"
#include <stdio.h>
#include <string.h>

#define EOS_FAKE ((void *)0xdeadbeef)
typedef void (*EosCb_t)(void *info);
static void *eos_auth_data;
static EosCb_t eos_auth_cb;

static int eos_ok(void) { return 0; }
static void eos_nothing(void) {}
static void *eos_fake(void) { return EOS_FAKE; }
static int eos_true(void) { return 1; }
static int eos_logged_in(void) { return 2; }
static int eos_not_found(void) { return 18; }

static int eos_puid_to_string(void *id, char *out, int32_t *len)
{
    char s[24];
    int n = snprintf(s, sizeof s, "%llu", (unsigned long long)fake_self);
    if (*len < n + 1)
        return 17;
    memcpy(out, s, n + 1);
    *len = n + 1;
    return 0;
}

static int eos_bytes_to_string(const uint8_t *p, uint32_t n, char *out, uint32_t *len)
{
    if (n + 1 <= *len)
        memcpy(out, p, n), out[n] = 0;
    *len = n + 1;
    return 0;
}

static void eos_login(void *h, void *opts, void *data, EosCb_t cb)
{
    struct { int32_t result; void *data, *user, *token; } info = {0, data, EOS_FAKE, NULL};
    log_event("EOS", NULL, "Connect_Login", NULL, 0);
    cb(&info);
}

static void *eos_add_auth_notify(void *h, void *opts, void *data, EosCb_t cb)
{
    eos_auth_data = data, eos_auth_cb = cb;
    return EOS_FAKE;
}

static int eos_register_peer(void *h, const uint8_t *opts)
{
    struct { void *data, *peer; int32_t status; } info = {eos_auth_data, *(void **)(opts + 8), 2};
    if (eos_auth_cb)
        eos_auth_cb(&info);
    return 0;
}

static const struct { const char *name; void *fn; } stubs[] = {
    {"EOS_ByteArray_ToString", eos_bytes_to_string},
    {"EOS_ProductUserId_IsValid", eos_true},
    {"EOS_ProductUserId_ToString", eos_puid_to_string},
    {"EOS_ProductUserId_FromString", eos_fake},
    {"EOS_Initialize", eos_ok},
    {"EOS_Shutdown", eos_ok},
    {"EOS_Platform_Create", eos_fake},
    {"EOS_Platform_Release", eos_nothing},
    {"EOS_Platform_Tick", eos_nothing},
    {"EOS_Platform_GetConnectInterface", eos_fake},
    {"EOS_Platform_GetAntiCheatClientInterface", eos_fake},
    {"EOS_Logging_SetCallback", eos_ok},
    {"EOS_Logging_SetLogLevel", eos_ok},
    {"EOS_Connect_Login", eos_login},
    {"EOS_Connect_CreateUser", eos_nothing},
    {"EOS_Connect_GetLoginStatus", eos_logged_in},
    {"EOS_Connect_AddNotifyAuthExpiration", eos_fake},
    {"EOS_Connect_RemoveNotifyAuthExpiration", eos_nothing},
    {"EOS_Connect_AddNotifyLoginStatusChanged", eos_fake},
    {"EOS_Connect_RemoveNotifyLoginStatusChanged", eos_nothing},
    {"EOS_AntiCheatClient_AddNotifyMessageToPeer", eos_fake},
    {"EOS_AntiCheatClient_RemoveNotifyMessageToPeer", eos_nothing},
    {"EOS_AntiCheatClient_AddNotifyPeerActionRequired", eos_fake},
    {"EOS_AntiCheatClient_RemoveNotifyPeerActionRequired", eos_nothing},
    {"EOS_AntiCheatClient_AddNotifyPeerAuthStatusChanged", eos_add_auth_notify},
    {"EOS_AntiCheatClient_RemoveNotifyPeerAuthStatusChanged", eos_nothing},
    {"EOS_AntiCheatClient_BeginSession", eos_ok},
    {"EOS_AntiCheatClient_EndSession", eos_ok},
    {"EOS_AntiCheatClient_PollStatus", eos_not_found},
    {"EOS_AntiCheatClient_RegisterPeer", eos_register_peer},
    {"EOS_AntiCheatClient_ReceiveMessageFromPeer", eos_ok},
    {"EOS_AntiCheatClient_UnregisterPeer", eos_ok},
};
#define EOS_DLL "EOSSDK-Win64-Shipping.dll"
static struct { uintptr_t *slot, game; void *stub; } gates[sizeof stubs / sizeof *stubs];
static int gate_count;

// On the first EOS call, once every mod has loaded, points every gated slot at its stub, or back at the game's
// own entry when a mod already loaded the SDK to handle EOS itself. Returns the entry for gate i.
static __attribute__((used)) void *eos_target(uintptr_t i)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static bool stub;
    BOOL pending;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        stub = !GetModuleHandleA(EOS_DLL);
        if (stub)
            log_line(LOG_INFO, "EOS stubbed, no mod loaded the SDK");
        else
            log_line(LOG_INFO, "EOS passes through, a mod loaded the SDK");
        for (int k = 0; k < gate_count; k++) {
            DWORD old;
            VirtualProtect(gates[k].slot, 8, PAGE_READWRITE, &old);
            *gates[k].slot = stub ? (uintptr_t)gates[k].stub : gates[k].game;
            VirtualProtect(gates[k].slot, 8, old, &old);
        }
        InitOnceComplete(&once, 0, NULL);
    }
    return stub ? gates[i].stub : (void *)gates[i].game;
}

// Entered from a slot's thunk with the gate number in r11; keeps the call's arguments and jumps to the entry.
void eos_gate(void);
__asm__(".text\n"
        "eos_gate:\n"
        "  push %rcx\n  push %rdx\n  push %r8\n  push %r9\n"
        "  sub $104, %rsp\n"
        "  movdqu %xmm0, 32(%rsp)\n  movdqu %xmm1, 48(%rsp)\n  movdqu %xmm2, 64(%rsp)\n  movdqu %xmm3, 80(%rsp)\n"
        "  mov %r11, %rcx\n"
        "  call eos_target\n"
        "  movdqu 32(%rsp), %xmm0\n  movdqu 48(%rsp), %xmm1\n  movdqu 64(%rsp), %xmm2\n  movdqu 80(%rsp), %xmm3\n"
        "  add $104, %rsp\n"
        "  pop %r9\n  pop %r8\n  pop %rdx\n  pop %rcx\n"
        "  jmp *%rax\n");

// Points every stubbed EOSSDK delay-import slot at a thunk into eos_gate; returns how many.
int gate_eos(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    uint8_t *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int total = 0;
    for (IMAGE_DELAYLOAD_DESCRIPTOR *d = (void *)(base + dir.VirtualAddress);
         code && dir.VirtualAddress && d->DllNameRVA; d++) {
        if (_stricmp((char *)base + d->DllNameRVA, EOS_DLL))
            continue;
        IMAGE_THUNK_DATA *names = (void *)(base + d->ImportNameTableRVA);
        uintptr_t *slots = (void *)(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData; names++, slots++, total++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            // A slot that no longer points into the game belongs to a mod that loaded before this one.
            if (*slots - (uintptr_t)base >= nt->OptionalHeader.SizeOfImage)
                continue;
            const char *fn = ((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            for (size_t i = 0; i < sizeof stubs / sizeof *stubs; i++) {
                if (strcmp(fn, stubs[i].name))
                    continue;
                // mov r11, gate number; mov rax, eos_gate; jmp rax
                uint8_t *t = code + gate_count * 32;
                uint64_t n = gate_count, g = (uintptr_t)eos_gate;
                t[0] = 0x49, t[1] = 0xbb, memcpy(t + 2, &n, 8);
                t[10] = 0x48, t[11] = 0xb8, memcpy(t + 12, &g, 8);
                t[20] = 0xff, t[21] = 0xe0;
                gates[gate_count].slot = slots, gates[gate_count].game = *slots, gates[gate_count].stub = stubs[i].fn;
                gate_count++;
                DWORD old;
                VirtualProtect(slots, 8, PAGE_READWRITE, &old);
                *slots = (uintptr_t)t;
                VirtualProtect(slots, 8, old, &old);
            }
        }
    }
    log_line(gate_count ? LOG_DEBUG : LOG_ERROR, "EOS gates set=%d of %d", gate_count, total);
    return gate_count;
}
