# Agent notes

Rekindle is a Windows DLL for Elden Ring Nightreign, written in C and cross-compiled with MinGW. Read README.md first for what it does and how the parts fit.

## Build

- `nix run .` builds `rekindle.dll` in the repo root. `nix build` builds it into `./result`.
- Inside `nix develop`, run `make`. The bare MinGW gcc wrapper needs `-L<mcfgthreads>/lib`, which the flake app adds for you.
- `make` must build without warnings (`-Wall`).
- `rekindle.dll` and `result` are ignored. Never commit them.

## Code

- `rekindle/` and `snapshot/` build into one DLL. Calls between them go through `common/modules.h`, and nothing else crosses that line.
- Game addresses are RVAs into `nightreign.exe` (base plus offset). Shared ones live in `common/game.h`. Name what an offset holds in a short comment.
- Snapshot record ids and data file names live in `common/snapshot_format.h`. Data files start with `rekindle_` and sit next to the DLL (`mod_path`).
- Settings are read with `cfg(section, key, default)`. A new key also needs a line in README.md and in the ini that ships with the DLL.
- Log with `log_line(level, ...)` into `rekindle.log`. Keep debug output at `LOG_DEBUG`.
- Comments are at most two lines and only explain what is not obvious from the code.
- Match the existing style: dense C, early returns, fixed buffers where the size is known.

## Safety

- In local mode (no `cl_server_redirector.dll`), every WinHTTP connection must go to the loopback server. A change must never let a request reach FromSoftware servers.
- Local mode turns on by itself. Do not add a setting for it.
- Rekindle writes to the player's save. Keep the `.pre_restore` backup before any write to `NR0000.sl2`.

## Testing

There are no unit tests. Test in game:

1. Load the DLL early from an me3 profile and set `[log] level=info` or `debug`.
2. Start a run, wait for a snapshot in the log, then kill the game process.
3. Relaunch, rejoin, and check the log and the world (items, enemy HP, runes, graces).
4. For local mode, use a profile without `cl_server_redirector.dll`.

Afterwards restore the real save from its backup and set the log level back to `error`.

## Commits

- Use Conventional Commits: `feat:`, `fix:`, `refactor:`, `build:`, `docs:`, `chore:`.
- The subject is lowercase, imperative and has no trailing period.
