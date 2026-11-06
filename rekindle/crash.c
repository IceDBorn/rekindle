#include "rekindle.h"
#include <stdio.h>
#include <string.h>

// "module+0xoffset" for an address inside a loaded module, the bare address otherwise.
static void where(char *out, size_t cap, uintptr_t a)
{
    HMODULE m;
    char path[MAX_PATH];
    if (a > 0x10000 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) &&
        GetModuleFileNameA(m, path, sizeof path)) {
        const char *b = strrchr(path, '\\');
        snprintf(out, cap, "%s+0x%llx", b ? b + 1 : path, (unsigned long long)(a - (uintptr_t)m));
    } else {
        snprintf(out, cap, "0x%llx", (unsigned long long)a);
    }
}

// The DLRegularHeap panic at +0x214e298 keeps the free's rbx (heap) and rdi (block - 0x10) on the stack, and its header
// in rbp. Plausible size with bit 0 clear means a double free, garbage means an overwrite.
static void log_heap_panic(const CONTEXT *c)
{
    HANDLE me = GetCurrentProcess();
    uintptr_t block = 0, heap = 0, msg = 0, range[2] = {0};
    char text[96] = "", hex[3 * 96 + 1] = "";
    uint8_t mem[96];
    SIZE_T got = 0;
    ReadProcessMemory(me, (void *)(c->Rsp + 0x58), &block, sizeof block, NULL);
    ReadProcessMemory(me, (void *)(c->Rsp + 0x60), &heap, sizeof heap, NULL);
    ReadProcessMemory(me, (void *)(c->Rsp + 0x80), &msg, sizeof msg, NULL);
    ReadProcessMemory(me, (void *)(heap + 8), range, sizeof range, NULL);
    ReadProcessMemory(me, (void *)msg, text, sizeof text - 1, NULL);
    ReadProcessMemory(me, (void *)(block - 0x20), mem, sizeof mem, &got);
    for (SIZE_T i = 0; i < got; i++)
        snprintf(hex + i * 3, 4, "%02x ", mem[i]);
    log_line(LOG_ERROR, "Heap panic \"%s\" heap=%p range=%p-%p block=%p header=%llx from block-0x20: %s", text,
             (void *)heap, (void *)range[0], (void *)range[1], (void *)block, (unsigned long long)c->Rbp, hex);
}

// Logs fatal-looking exceptions, since the game exits without a dialog. It runs before any handler, so a line can
// also come from an exception the game handles and survives.
LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep)
{
    static volatile LONG count;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != 0xc0000374 &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    if (log_level < LOG_ERROR || InterlockedIncrement(&count) > 20)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t size = ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    CONTEXT *c = ep->ContextRecord;
    uintptr_t ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    char line[4096], at[MAX_PATH + 24];
    where(at, sizeof at, ip);
    int n = snprintf(line, sizeof line, "CRASH code=%08lx at=%s ip=%p tid=%lu addr=%p rcx=%llx rdx=%llx rax=%llx",
                     code, at, (void *)ip, GetCurrentThreadId(),
                     (void *)ep->ExceptionRecord->ExceptionInformation[1], (unsigned long long)c->Rcx,
                     (unsigned long long)c->Rdx, (unsigned long long)c->Rax);
#define ADD(...) (n += n < (int)sizeof line ? snprintf(line + n, sizeof line - n, __VA_ARGS__) : 0)
    // Real call chain via unwind data, then the regs that usually hold `this`, with their vtable rva.
    CONTEXT u = *c;
    ADD(" unwind:");
    for (int i = 0; i < 24 && u.Rip; i++) {
        DWORD64 ib;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &ib, NULL);
        where(at, sizeof at, u.Rip);
        ADD(" %s", at);
        if (!fn)
            break;
        void *hd;
        DWORD64 ef;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u.Rip, fn, &u, &hd, &ef, NULL);
    }
    DWORD64 regs[] = {c->Rbx, c->Rsi, c->Rdi, c->R8, c->R12, c->R13, c->R14, c->R15};
    ADD(" regs:");
    for (int i = 0; i < 8; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t vt = 0;
        if (regs[i] > 0x10000 && VirtualQuery((void *)regs[i], &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            vt = *(uintptr_t *)regs[i];
        ADD(" %llx/%llx", (unsigned long long)regs[i], (unsigned long long)(vt - base < size ? vt - base : 0));
    }
    ADD(" stack:");
    // Stop at the end of the committed stack region; reading past the stack top faults inside this handler.
    uintptr_t *sp = (uintptr_t *)c->Rsp;
    MEMORY_BASIC_INFORMATION mbi;
    int words = VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT
                    ? (int)(((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)sp) / 8) : 0;
    for (int i = 0, k = 0; i < 0x300 && i < words && k < 24; i++) {
        if (sp[i] - base < size)
            ADD(" 0x%llx", (unsigned long long)(sp[i] - base)), k++;
    }
#undef ADD
    log_line(LOG_ERROR, "%s", line);
    if (ip - base == 0x214e298)
        log_heap_panic(c);
    return EXCEPTION_CONTINUE_SEARCH;
}
