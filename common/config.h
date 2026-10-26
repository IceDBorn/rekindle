#pragma once

// Settings from rekindle.ini next to the DLL; a missing file or key keeps the default.
int cfg(const char *section, const char *key, int def);
