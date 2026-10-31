// Rekindle: restores crashed runs. At log level debug it also logs the game's WinHTTP and Steam calls.
// Hooks nightreign.exe's import table, so it sees requests before cl_server_redirector rewrites them.
#include "rekindle.h"
#include <string.h>

static int patch_table(IMAGE_THUNK_DATA *names, IMAGE_THUNK_DATA *slots, uint8_t *base,
                       const struct import_hook *table, int count)
{
    int patched = 0;
    for (; names->u1.AddressOfData; names++, slots++) {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
            continue;
        const char *fn = ((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
        for (int i = 0; i < count; i++) {
            if (strcmp(fn, table[i].name))
                continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old);
            *table[i].real = (void *)(uintptr_t)slots->u1.Function;
            slots->u1.Function = (uintptr_t)table[i].hook;
            VirtualProtect(&slots->u1.Function, 8, old, &old);
            patched++;
        }
    }
    return patched;
}

static void patch_imports(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir.VirtualAddress); dir.VirtualAddress && d->Name; d++) {
        bool http = !_stricmp((char *)base + d->Name, "winhttp.dll");
        if (!http && _stricmp((char *)base + d->Name, "steam_api64.dll"))
            continue;
        IMAGE_THUNK_DATA *names = (void *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *slots = (void *)(base + d->FirstThunk);
        patched += http ? patch_table(names, slots, base, http_imports, http_import_count)
                        : patch_table(names, slots, base, steam_imports, steam_import_count);
    }
    log_line(patched ? LOG_DEBUG : LOG_ERROR, "Import hooks patched=%d of %d", patched,
             http_import_count + steam_import_count);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(self);
    InitializeCriticalSection(&emu_lock);
    mod_dir_init(self);
    cfg_write_default();
    log_init("rekindle.log");
    fake_lobby = DEFAULT_LOBBY, fake_host = DEFAULT_HOST;
    CloseHandle(CreateThread(NULL, 0, auto_restore, NULL, 0, NULL));
    AddVectoredExceptionHandler(1, crash_veh);
    CloseHandle(CreateThread(NULL, 0, startup_patches, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, ping_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, hub_thread, NULL, 0, NULL));
    patch_imports();
    snapshot_start();
    // Local mode needs this DLL loaded early, before the game's EOS setup runs.
    if (!GetModuleHandleA("cl_server_redirector.dll"))
        patch_eos();
    return TRUE;
}
