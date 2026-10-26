#pragma once
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

// The folder holding the DLLs, with a trailing separator; settings, logs and run files live there.
extern char dll_dir[MAX_PATH];
extern int dll_dir_len;

uint64_t unix_ms(void);
void mod_dir_init(HMODULE self);
void mod_path(char out[MAX_PATH], const char *fmt, ...);
uint8_t *read_all(const char *path, size_t *len);
const uint8_t *find_bytes(const uint8_t *s, size_t n, const char *needle);
