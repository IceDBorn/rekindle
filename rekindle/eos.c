// EOS stubs: in local mode the game's EOS delay imports point here, so the SDK and EAC never load.
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

// Points every EOSSDK delay-import slot at a stub; returns how many were patched.
int patch_eos(void)
{
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
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    int patched = 0, total = 0;
    for (IMAGE_DELAYLOAD_DESCRIPTOR *d = (void *)(base + dir.VirtualAddress); dir.VirtualAddress && d->DllNameRVA; d++) {
        if (_stricmp((char *)base + d->DllNameRVA, "EOSSDK-Win64-Shipping.dll"))
            continue;
        IMAGE_THUNK_DATA *names = (void *)(base + d->ImportNameTableRVA);
        uintptr_t *slots = (void *)(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData; names++, slots++, total++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const char *fn = ((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            for (size_t i = 0; i < sizeof stubs / sizeof *stubs; i++) {
                if (strcmp(fn, stubs[i].name))
                    continue;
                DWORD old;
                VirtualProtect(slots, 8, PAGE_READWRITE, &old);
                *slots = (uintptr_t)stubs[i].fn;
                VirtualProtect(slots, 8, old, &old);
                patched++;
            }
        }
    }
    log_line(patched ? LOG_INFO : LOG_ERROR, "Local mode: EOS stubs patched=%d of %d", patched, total);
    return patched;
}
