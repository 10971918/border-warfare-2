# Border Warfare 2

**Play Borderlands 2 with Call of Duty: Modern Warfare 2's movement, aiming and gunplay.**

Modern Warfare 2 (2009, single-player) runs in the background and supplies how you move, aim, aim down sights,
fire, reload and throw grenades. Borderlands 2 shows the world, runs the enemies and applies the damage, with loot
and XP as normal. MW2's arms and gun are drawn over Borderlands 2, so it plays like an MW2 character dropped into
Pandora.

No game files are included or modified. You need your own Steam copies of both games.

## Status

**Playable.** Tested end to end on Windows 11 with the Steam versions of both games. What works:

- MW2 movement on Borderlands 2's ground and walls: walk, sprint, jump, crouch, prone, stairs.
- MW2's first-person arms, gun, crosshair and ammo counter drawn over Borderlands 2.
- MW2 shots damage Borderlands 2 enemies, using your equipped BL2 gun's damage; headshots do double. MW2 frags
  throw your BL2 grenade.
- Weapon switching, reloading, unlimited MW2 ammo, a knife.
- Shield and health bars with a damage flash.
- **F7** to switch MW2 control on and off; **F8** for third person (your BL2 character, crouch included).
- Using vehicle stations, doors, vending machines and chests (hold X). Getting into a vehicle hands control
  back to Borderlands 2, and MW2 control returns when you get out.
- Offline test suite (no games needed).

Known gaps and ideas for what to build next are in [STATUS.md](STATUS.md).

## Requirements

- Windows 10/11, an Xbox-style controller.
- **Borderlands 2** (Steam) with the community [BL2 Python SDK](https://github.com/bl-sdk) v3.x installed.
- **Call of Duty: Modern Warfare 2** (2009, Steam). The supported build is the 64-bit `iw4sp.exe` with
  TimeDateStamp `0x6aaaa257`. Other builds are detected and the MW2 side switches itself off.
- To build: Visual Studio 2022 Build Tools (C++ desktop workload). To run the tests: 64-bit Python 3.10+.

## Install and play

See **[INSTALL.md](INSTALL.md)**: build, install, controls, settings, troubleshooting and uninstall.

In short:
1. Edit `paths.bat` for where your games are installed.
2. Run `backup_saves.bat` once.
3. Run `build.bat`, then `install.bat`.
4. Start BL2, start MW2 (it loads its mission by itself), then press **F7** in BL2.

`uninstall.bat` removes everything.

## How it works

Two games run side by side and talk through shared memory (`protocol/bw2_protocol.h`).

| Part | Where | Job |
|---|---|---|
| **MW2 guest** | `src/mw_guest/` → `winmm.dll` (x64) next to `iw4sp.exe` | Hooks MW2's movement (Pmove), input (CL_CreateCmd) and bullets. Makes MW2's movement collide with Borderlands 2's world instead of its own, keeps MW2's player in empty space below its level, and captures each frame's arms and gun (magenta background keyed to transparent). |
| **BL2 host** | `sdk_mods/bw2/` (Python SDK mod) | Scans Borderlands 2's collision around you into boxes for MW2, moves your BL2 character to wherever MW2's player is, turns MW2's bullets into BL2 damage, and handles F7/F8, use, vehicles and the HUD. |
| **BL2 overlay** | `src/bl_overlay/` → `bw2_overlay.dll` (x86) | Draws MW2's captured frame and the health bars over Borderlands 2. |
| **Native scanner** | `src/bl_scan/` → `bw2_scan.dll` (x86) | Fast version of the collision scanner. |

Your BL2 character is a puppet: Borderlands 2 still owns the world, enemies, damage, loot and saves, while MW2
only provides the feel. More detail, including every hard-won finding, is in [PROGRESS.md](PROGRESS.md) and
[recon/PHASE0.md](recon/PHASE0.md).

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Native code: MW2 guest, BL2 overlay, native scanner, shared helpers |
| `sdk_mods/bw2/` | The Borderlands 2 SDK mod |
| `protocol/` | Shared-memory protocol between the two games |
| `tests/` | Offline tests: C++ (`*_test.cpp`) and Python (`test_*.py`) |
| `tools/` | Fake MW2/BL2 for offline testing, a live position recorder, read-only memory peek, reverse-engineering scripts for your own `iw4sp.exe` (`tools/re/`) |
| `recon/`, `recon_dll/`, `sdk_mods/bw2_recon/` | The original feasibility study and its read-only probes |
| `*.bat` | Build, test, install, uninstall, back up saves, game paths |

## Contributing

Contributions are welcome. See **[CONTRIBUTING.md](CONTRIBUTING.md)** for setup, tests and the project's ground
rules, and [STATUS.md](STATUS.md) for open work.

## Legal

Unofficial fan project, not affiliated with or endorsed by Activision, Infinity Ward, Gearbox Software or 2K.
Single-player only: no online play is touched. No game files, assets or decompiled code are distributed.

Licensed under the [MIT License](LICENSE). Design credits are in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
