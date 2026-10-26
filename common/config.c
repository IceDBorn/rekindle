#include "config.h"
#include "snapshot_format.h"
#include "util.h"

int cfg(const char *section, const char *key, int def)
{
    char path[MAX_PATH];
    mod_path(path, INI_FILE);
    return (int)GetPrivateProfileIntA(section, key, def, path);
}
