#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Log levels from [log] level in rekindle.ini; the file is created on the first line written.
enum { LOG_OFF, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };

extern CRITICAL_SECTION log_lock;
extern int log_level;
extern uint64_t log_t0;

void log_init(const char *file);
FILE *log_begin(int level);
void log_end(void);
void log_line(int level, const char *fmt, ...);
void log_event(const char *what, void *h, const char *info, const void *data, size_t len);
void log_dedup(const char *what, const char *line);
void note(int level, const char *fmt, ...);
void log_mismatch(const void *at, const char *name);
