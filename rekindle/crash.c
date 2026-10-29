#include "rekindle.h"
#include <stdio.h>

// Logs fatal-looking exceptions with exe RVAs found on the stack, since the game exits without a dialog.
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
    char line[2048];
    int n = snprintf(line, sizeof line, "CRASH code=%08lx ip=%p rva=0x%llx addr=%p rcx=%llx rdx=%llx rax=%llx", code,
                     (void *)ip, (unsigned long long)(ip - base < size ? ip - base : 0),
                     (void *)ep->ExceptionRecord->ExceptionInformation[1], (unsigned long long)c->Rcx,
                     (unsigned long long)c->Rdx, (unsigned long long)c->Rax);
#define ADD(...) (n += n < (int)sizeof line ? snprintf(line + n, sizeof line - n, __VA_ARGS__) : 0)
    // Real call chain via unwind data, then the regs that usually hold `this`, with their vtable rva.
    CONTEXT u = *c;
    ADD(" unwind:");
    for (int i = 0; i < 24 && u.Rip; i++) {
        DWORD64 ib;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &ib, NULL);
        ADD(" 0x%llx", (unsigned long long)(u.Rip - base < size ? u.Rip - base : u.Rip));
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
    return EXCEPTION_CONTINUE_SEARCH;
}
