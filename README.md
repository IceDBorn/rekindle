# Rekindle

Rekindle is a native mod for Elden Ring Nightreign that brings back a run after the game crashes. Relaunch the game and it offers to rejoin the run, with the map, items and enemies as they were a few seconds before the crash.

It works for solo runs and co-op runs, after a crash or a quit to the desktop or title screen, and offline. Quitting to the title screen still needs a relaunch before the restore.

## How it works

- During a run, Rekindle writes a snapshot of the host's world every 30 seconds to `rekindle_run_<multi_game_id>.bin`. A snapshot is what the game would send a player joining at that moment. `rekindle_active_run.txt` names the run in progress, and only the end of the run clears it, so a quit keeps the run restorable. `rekindle_mates.txt` lists the teammates and the run's Steam lobby.
- On the next launch, if a run was still in progress, Rekindle adds a rejoin record to `NR0000.sl2`, so the game offers to resume the run. It backs up the save to `NR0000.sl2.pre_restore` first.
- In co-op, the restore first checks the run's lobby. If a teammate is still in the run, the game rejoins them as usual.
- The rejoin goes to a fake lobby. A fake host answers the game's P2P link with the snapshot, then leaves like a crashed host, and the player keeps the run as its new host.
- After the join, Rekindle puts back what a join does not carry. That is ground items, runes earned since the last save, the spawn position, the player number (so bosses drop dormant powers), field boss graces, enemy and boss HP, and the time of day.
- When every player left, the first player to restore creates a lobby tagged with the run id. The others find it by lobby search and wait until the first player's world has loaded, then join.
- Teammate names show the ping and packet loss to that player. Rekindle sends a small ping with the player's in-game name on its own P2P channel every 500 ms, and the teammate's Rekindle sends it back. Loss comes from gaps in the sequence numbers of the game's own packets, so it shows even when the teammate has no Rekindle.
- Experimental, off by default: players in one game party see each other in the Roundtable Hold. The party leader opens a game session and writes its lobby id into the party lobby, and the other members join it. Rekindle then runs the join steps a map load would run, gives each player its own player number, and hands the sparring puppet to the leader. The ready count lives in that session, so the session closes at the first ready press. That press is lost, and the player presses ready again.
- It also removes the 300 second AFK kick and the rejoin penalty, and a player who crashed while downed comes back standing.

Without `cl_server_redirector.dll` loaded, Rekindle answers every game server request itself from a loopback server and stubs EOS. The game never contacts FromSoftware servers in that mode.

## Install

Rekindle loads with [me3](https://github.com/garyttierney/me3). Put `rekindle.dll` and `rekindle.ini` in one folder and add the DLL to your profile as an early native:

```toml
[[natives]]
path = "dll/rekindle.dll"
load_early = true
```

Rekindle reads `rekindle.ini` from the folder that holds the DLL and writes its data files and `rekindle.log` there too. Missing keys keep their defaults.

The game addresses are for one build of `nightreign.exe`. A game update can break Rekindle until the addresses are updated.

## Settings

| Section | Key | Default | Effect |
| --- | --- | --- | --- |
| restore | auto_restore | 1 | Restore a crashed run on the next launch. |
| restore | keep_online | 1 | Keep a restored run online after the fake host leaves. |
| restore | restore_ground_items | 1 | Put items left on the ground back. |
| restore | restore_runes | 1 | Give back runes earned after the game last wrote the save. |
| restore | restore_position | 1 | Use the snapshot position when the save has none, for a crash in the first seconds of a run. |
| restore | fix_boss_drops | 1 | Give the restored player its own number from the run again, so bosses drop dormant powers. |
| restore | fix_graces | 1 | Light field boss graces again after reload events switch them off. |
| restore | restore_enemy_hp | 1 | Put back the HP of damaged enemies and bosses that were alive in the snapshot. |
| penalties | remove_afk_kick | 1 | Remove the 300 second idle kick. |
| penalties | remove_rejoin_penalty | 1 | Rejoining after a crash costs no level and drops no runes. |
| penalties | respawn_if_downed | 1 | A player who crashed while downed comes back standing. |
| snapshots | first_after_s | 10 | Seconds into a run before the first snapshot. |
| snapshots | every_s | 30 | Seconds between snapshots. |
| snapshots | resume_after_restore_s | 60 | Seconds after a restore before snapshots start again. |
| snapshots | stop_before_boss_s | 60 | Real seconds before a boss phase when snapshots stop, so a restore leaves time to rejoin. The boss restarts. |
| ping | show_ping | 1 | Show the round trip time next to teammate names. Only teammates who also run Rekindle answer. |
| ping | show_loss | 1 | Show packet loss from a teammate next to their name when it reaches 1%. |
| ping | hud_labels | 1 | Show ping and loss in the HUD party list. |
| ping | overhead_labels | 0 | Show ping and loss in the name labels above teammates. |
| hub | shared_roundtable | 0 | Experimental. Players in one game party see each other in the Roundtable Hold. Every party member needs Rekindle. The first ready press closes the shared view and has to be repeated. |
| log | level | error | `off`, `error`, `warn`, `info` or `debug`. Debug also logs the game's WinHTTP and Steam calls. |

## Build

With Nix:

```sh
nix run .      # builds rekindle.dll in this folder
nix build      # builds it into ./result
nix develop    # shell with the MinGW toolchain, then run make
```

Without Nix, any MinGW-w64 GCC works:

```sh
make CC=x86_64-w64-mingw32-gcc
```

The DLL links statically and imports only Windows system DLLs.

## Layout

- `common/` has the log, settings, import patching, game addresses and the snapshot file format. `modules.h` declares the calls between the two halves.
- `rekindle/` is the restore side. It has the save planting, the fake lobby and fake host, the co-op relay, the local server, the WinHTTP and Steam hooks, and the game patches.
- `snapshot/` takes the snapshots and puts back the rest of the world after a restore.

## License

AGPL-3.0. See [LICENSE](LICENSE).
