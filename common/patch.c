#include <windows.h>
#include <string.h>
#include "log.h"
#include "patch.h"

void *alloc_near(uint8_t *base)
{
    for (uint8_t *p = base - 0x10000000; p > base - 0x70000000; p -= 0x10000) {
        void *m = VirtualAlloc(p, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (m)
            return m;
    }
    return NULL;
}

// Points the call rel32 at call_rva to hook through a stub near the image, if it still calls fn_rva.
bool patch_call(uint32_t call_rva, uint32_t fn_rva, void *hook, const char *name)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    uint8_t *call = base + call_rva;
    int32_t rel;
    memcpy(&rel, call + 1, 4);
    if (call[0] != 0xe8 || call + 5 + rel != base + fn_rva) {
        log_mismatch(call, name);
        return false;
    }
    uint8_t *stub = alloc_near(base);
    if (!stub)
        return false;
    stub[0] = 0x48, stub[1] = 0xb8;
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xff, stub[11] = 0xe0;
    rel = (int32_t)(stub - (call + 5));
    DWORD old;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(call + 1, &rel, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

// Replaces a position-independent prologue of n >= 14 bytes with an absolute jump; returns the trampoline.
void *patch_prologue(uint32_t rva, const uint8_t *expect, int n, void *target, const char *name)
{
    uint8_t *fn = (uint8_t *)GetModuleHandleA(NULL) + rva;
    if (memcmp(fn, expect, n)) {
        log_mismatch(fn, name);
        return NULL;
    }
    uint8_t *tramp = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return NULL;
    memcpy(tramp, fn, n);
    tramp[n] = 0xff, tramp[n + 1] = 0x25;
    memset(tramp + n + 2, 0, 4);
    uint8_t *back = fn + n;
    memcpy(tramp + n + 6, &back, 8);
    uint8_t jmp[14] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(jmp + 6, &target, 8);
    DWORD old;
    if (!VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old))
        return NULL;
    memcpy(fn, jmp, 14);
    VirtualProtect(fn, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 14);
    return tramp;
}

// Writes new over old at address at, only while the original bytes are still there.
bool patch_bytes(uintptr_t at, const uint8_t *old_b, const uint8_t *new_b, size_t n)
{
    uint8_t *p = (uint8_t *)at;
    DWORD old;
    if (memcmp(p, old_b, n) || !VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(p, new_b, n);
    VirtualProtect(p, n, old, &old);
    return true;
}
