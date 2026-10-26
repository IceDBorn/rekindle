#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

char dll_dir[MAX_PATH];
int dll_dir_len;

uint64_t unix_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000;
}

void mod_dir_init(HMODULE self)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    memcpy(dll_dir, path, n);
    dll_dir_len = (int)n;
}

// Writes dll_dir followed by the formatted file name.
void mod_path(char out[MAX_PATH], const char *fmt, ...)
{
    int n = snprintf(out, MAX_PATH, "%.*s", dll_dir_len, dll_dir);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, MAX_PATH - n, fmt, ap);
    va_end(ap);
}

uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz > 0 ? sz : 1);
    *len = fread(b, 1, sz, f);
    fclose(f);
    return b;
}

const uint8_t *find_bytes(const uint8_t *s, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; k <= n && i <= n - k; i++)
        if (!memcmp(s + i, needle, k))
            return s + i;
    return NULL;
}
