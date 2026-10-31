#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "snapshot_format.h"
#include "util.h"

// rekindle.ini from the repo root, built in so a missing file can be written out.
__asm__(".section .rdata,\"dr\"\ndefault_ini:\n.incbin \"" INI_FILE "\"\n.byte 0\n.text\n");
extern const char default_ini[];

int cfg(const char *section, const char *key, int def)
{
    char path[MAX_PATH];
    mod_path(path, INI_FILE);
    return (int)GetPrivateProfileIntA(section, key, def, path);
}

void cfg_write_default(void)
{
    char path[MAX_PATH];
    mod_path(path, INI_FILE);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    size_t len = strlen(default_ini), k = 0;
    char *crlf = malloc(len * 2);
    if (crlf) {
        for (size_t i = 0; i < len; i++) {
            if (default_ini[i] == '\n' && (!i || default_ini[i - 1] != '\r'))
                crlf[k++] = '\r';
            crlf[k++] = default_ini[i];
        }
        DWORD n;
        WriteFile(h, crlf, (DWORD)k, &n, NULL);
        free(crlf);
    }
    CloseHandle(h);
}
