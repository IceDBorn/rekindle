#include <stdarg.h>
#include "log.h"
#include "snapshot_format.h"
#include "util.h"

CRITICAL_SECTION log_lock;
int log_level = LOG_ERROR;
uint64_t log_t0;
static const char *const level_names[] = {"OFF", "ERROR", "WARN", "INFO", "DEBUG"};
static char log_path[MAX_PATH];
static FILE *out;
static bool log_failed;

// Needs mod_dir_init first. The log only holds this launch: an old one is removed even when nothing gets written.
void log_init(const char *file)
{
    char ini[MAX_PATH], v[16];
    InitializeCriticalSection(&log_lock);
    log_t0 = unix_ms();
    mod_path(ini, INI_FILE);
    GetPrivateProfileStringA("log", "level", "error", v, sizeof v, ini);
    for (int i = 0; i < 5; i++)
        if (!_stricmp(v, level_names[i]))
            log_level = i;
    mod_path(log_path, "%s", file);
    // The last session's log survives one relaunch, so a crash log is not lost to an immediate restart.
    char prev[MAX_PATH + 8];
    snprintf(prev, sizeof prev, "%s.prev", log_path);
    MoveFileExA(log_path, prev, MOVEFILE_REPLACE_EXISTING);
}

// Takes the lock and writes the line prefix; returns NULL with the lock released when nothing is logged.
FILE *log_begin(int level)
{
    if (level > log_level || !log_path[0])
        return NULL;
    EnterCriticalSection(&log_lock);
    if (!out && !log_failed)
        log_failed = !(out = fopen(log_path, "w"));
    if (!out) {
        LeaveCriticalSection(&log_lock);
        return NULL;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(out, "%02d:%02d:%02d.%03d %-5s ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level_names[level]);
    return out;
}

void log_end(void)
{
    fflush(out);
    LeaveCriticalSection(&log_lock);
}

void log_line(int level, const char *fmt, ...)
{
    if (!log_begin(level))
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    log_end();
}

// Debug only: a header line, then the payload escaped so the log stays one text file.
void log_event(const char *what, void *h, const char *info, const void *data, size_t len)
{
    if (!log_begin(LOG_DEBUG))
        return;
    fprintf(out, "+%llu tid=%lu %s h=%p %s\n", (unsigned long long)(unix_ms() - log_t0), GetCurrentThreadId(), what,
            h, info ? info : "");
    if (data && len) {
        const uint8_t *p = data;
        fputs("  | ", out);
        for (size_t i = 0; i < len; i++) {
            if (p[i] == '\n')
                fputs(i + 1 < len ? "\n  | " : "", out);
            else if (p[i] == '\r')
                ;
            else if (p[i] >= 0x20 && p[i] < 0x7f && p[i] != '\\')
                fputc(p[i], out);
            else
                fprintf(out, "\\x%02x", p[i]);
        }
        fputc('\n', out);
    }
    log_end();
}

// Debug only: logs a line the first time and again at power-of-two repeat counts, so polling stays small.
void log_dedup(const char *what, const char *line)
{
    if (log_level < LOG_DEBUG)
        return;
    static uint64_t keys[8192];
    static uint32_t counts[8192];
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = what; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    for (const char *p = line; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    EnterCriticalSection(&log_lock);
    uint32_t i = h & 8191, c = 0;
    while (keys[i] && keys[i] != h)
        i = (i + 1) & 8191;
    keys[i] = h;
    c = ++counts[i];
    LeaveCriticalSection(&log_lock);
    if (c & (c - 1))
        return;
    char buf[1500];
    snprintf(buf, sizeof buf, c > 1 ? "%s (x%u)" : "%s", line, c);
    log_line(LOG_DEBUG, "%s %s", what, buf);
}

// Restore events; debug repeats are thinned out like the Steam call log.
void note(int level, const char *fmt, ...)
{
    if (level > log_level)
        return;
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (level == LOG_DEBUG)
        log_dedup("Fake", line);
    else
        log_line(level, "%s", line);
}

void log_mismatch(const void *at, const char *name)
{
    log_line(LOG_ERROR, "Patch failed: %s at rva=0x%llx", name,
             (unsigned long long)((uintptr_t)at - (uintptr_t)GetModuleHandleA(NULL)));
}
