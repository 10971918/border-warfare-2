# Contributing to Border Warfare 2

Thanks for helping. Open work is listed in [STATUS.md](STATUS.md). Pick something there, or open an issue first
for anything bigger, so we can agree on the approach before you write code.

## Ground rules

These keep the project legal, safe and installable for everyone. Pull requests that break them can't be merged.

1. **No game files.** Never commit anything from either game: exes, DLLs, maps, models, textures, sounds,
   dumps, decompiled code, or memory dumps. Addresses, structure offsets and short notes about how the games work
   are fine.
2. **Official Steam builds only.** No IW4x or other unofficial MW2 clients, and nothing that touches online play.
3. **Borderlands 2 through the community Python SDK** wherever it can do the job. Native code (`bw2_overlay.dll`,
   `bw2_scan.dll`) is only for what the SDK can't do.
4. **Everything installs with `install.bat` and comes out with `uninstall.bat`.** Never write to the games'
   files beyond what those scripts manage.
5. **Keep saves safe.** Nothing may move the BL2 character anywhere it could fall out of the world. Back up your
   saves (`backup_saves.bat`) before testing.
6. **Respect licences.** Only add third-party code with a compatible licence, and record it in
   [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Setup

1. Windows 10/11 with both games from Steam, and the BL2 Python SDK v3.x installed in Borderlands 2.
2. Visual Studio 2022 Build Tools with the C++ desktop workload. If yours is installed elsewhere, adjust `VS` at
   the top of `build.bat`.
3. A 64-bit Python 3.10+ for the tests and tools (standard library only). The reverse-engineering scripts in
   `tools/re/` also need `pip install capstone pefile numpy`.
4. Point `paths.bat` at your games.

## Build and test

```bat
build.bat        & rem builds everything into bin\ (MW2 side x64, BL2 side x86, test programs)
run_tests.bat    & rem runs every offline test; set PY=path\to\python.exe if "python" isn't on PATH
```

The offline tests need neither game:

| Test | Covers |
|---|---|
| `layout_test` (x64 and x86) | The shared-memory protocol has the same layout in both bitnesses |
| `field_test`, `walk_test` | MW2-side collision against BL2's scanned boxes; walking a course |
| `despeckle_test` | The snow filter on captured frames |
| `hook_loop_test` | The guard against the Steam overlay's Present hook loop (on a real D3D9 device) |
| `test_scan`, `test_native_scan` | The BL2 collision scanner, Python and native, box for box |
| `test_bl_mod` | The whole BL2 mod against a stand-in SDK and a fake MW2 (`tools/fake_mw2.py`): driving, gunplay, vehicles |

Please add or extend a test with every fix. Most real bugs in this project were reproduced in a test first.

## Testing in game

1. `install.bat` (or `install.bat mw` / `install.bat bl` while the other game stays open). It refuses to replace
   files a running game holds.
2. Logs go to `logs\mw_guest.log` and `logs\bl_host.log`. A status line is written every few seconds.
3. Live commands:
   - Write a line to `logs\bl_cmd.txt` for the BL2 mod: `status`, `drive on`, `drive off`, `god on`, `py <stmt>`
     and more (see `run_command` in `sdk_mods/bw2/__init__.py`).
   - Write a line to `logs\mw_cmd.txt` for the MW2 guest: `status`, `level <map>`, `setup`, `con <console
     command>` and more (see `RunCommand` in `src/mw_guest/main.cpp`).
4. `tools/jitter_probe.py` records MW2's and BL2's positions at about 1 kHz (read-only), to measure smoothness
   problems instead of guessing.

## Finding things in MW2

All MW2 addresses are relative to `iw4sp.exe` (64-bit Steam build, TimeDateStamp `0x6aaaa257`). They are
constants near the top of `src/mw_guest/main.cpp` and `src/mw_guest/capture.h`, each with a comment saying how it
was found. `tools/re/` has the scripts used: a PE and disassembly helper (`pe64.py`) and finders for callers,
dvar readers and code regions. They read your own copy of the exe. `tools/peek.py` reads a running game's memory
without changing it.

If Steam updates MW2, the guest logs an unknown build and stays off. Re-find the addresses with `tools/re/`,
update the constants and the known TimeDateStamp.

## Pull requests

- Keep each pull request to one change, and explain what it fixes or adds and how you tested it, in game or
  offline.
- `build.bat` and `run_tests.bat` must both pass.
- Match the surrounding code: its naming, comment style and level of detail. Comments explain *why*, especially
  for anything found by testing in game.
- Add a dated line to [PROGRESS.md](PROGRESS.md) for findings others will need, and update
  [STATUS.md](STATUS.md) when something on the roadmap is done.
