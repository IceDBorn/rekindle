#pragma once

// Settings from rekindle.ini next to the DLL; a missing file or key keeps the default.
int cfg(const char *section, const char *key, int def);
// Writes the built-in rekindle.ini when none exists. Needs mod_dir_init first.
void cfg_write_default(void);
