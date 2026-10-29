#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *alloc_near(uint8_t *base);
// True when n bytes at p are committed and readable; on Windows the exe stays unreadable while it unpacks.
bool readable(const void *p, size_t n);
// readable() plus a byte compare.
bool bytes_match(const void *p, const void *expect, size_t n);
bool patch_call(uint32_t call_rva, uint32_t fn_rva, void *hook, const char *name);
void *patch_prologue(uint32_t rva, const uint8_t *expect, int n, void *target, const char *name);
bool patch_bytes(uintptr_t at, const uint8_t *old_b, const uint8_t *new_b, size_t n);
