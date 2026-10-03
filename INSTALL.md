# Border Warfare 2: install and play

Play Borderlands 2 with Modern Warfare 2's movement, guns and feel. MW2 runs alongside BL2 and handles your
movement, aim, shooting and reloads. BL2 shows the world and runs the enemies, damage, loot and XP. You see
MW2's arms and gun drawn over BL2.

## What you need

- **Borderlands 2** and **Call of Duty: Modern Warfare 2 (2009)** on Steam, both installed.
- The **BL2 Python SDK** (v3.x) installed in Borderlands 2.
- An **Xbox-style controller**.
- To build from source: Visual Studio 2022 Build Tools (C++ desktop workload).

No game files are included or changed. Everything this mod adds can be removed with `uninstall.bat`.

## Install

1. If your games aren't in `A:\SteamLibrary\steamapps\common\...`, open `paths.bat` and fix both paths.
2. Run `backup_saves.bat` once. It copies your BL2 save folder to `backups\`.
3. Run `build.bat`. It should end with `BUILD OK`. Skip this if `bin\` already has the built files.
4. Quit both games, then run `install.bat`. It should end with `OK: installed Border Warfare 2`.

`install.bat` adds these files:

| Game | Files added |
|---|---|
| MW2 | `winmm.dll`, `bw2.ini` |
| BL2 | the `sdk_mods\bw2` mod folder |

## Play

1. Start **Borderlands 2** and load your character.
2. Start **Modern Warfare 2**. It loads the mission Contingency by itself and gives you an M4A1, a USP,
   frags and flashbangs. Wait until you're standing in the snow with the M4. Leave MW2 running in the
   background.
3. Click back into **Borderlands 2** and press **F7**. After a second, MW2's gun appears and BL2's HUD is
   replaced by:
   - MW2's ammo counter;
   - a shield bar (blue) and a health bar (red), bottom left.
4. Play with the controller. Press **F7** again to give control back to BL2.
5. Press **F8** to switch between first and third person. In third person you see your BL2 character, and MW2 only
   draws its crosshair and ammo counter.

Getting into a vehicle gives control back to BL2 (drive it with BL2's controls); when you get out, MW2
control comes back by itself.

If you go down (Fight For Your Life), control goes back to BL2 automatically. Once you're back up, press
**F7** again.

### Controls (controller, while F7 is on)

| Button | Action |
|---|---|
| Left stick / right stick | Move / look |
| LS click | Sprint (while aiming down sights: hold breath) |
| RT / LT | Fire / aim down sights |
| X | Reload |
| A | Jump (stands you up if crouched or prone) |
| B | Tap: crouch. Hold: prone |
| RS click | Knife |
| RB | Grenade (a BL2 grenade is thrown) |
| LB | Flashbang |
| Y | Switch weapon (M4A1 / USP) |
| X (hold) | Use: vehicle stations, doors, vending machines, chests (tap X = reload) |

Damage uses your **equipped BL2 gun's** damage (headshots do double). Kills, XP and loot are yours as
normal.

## Settings (`bw2.ini` in the MW2 folder, under `[BW2]`)

| Key | Default | Meaning |
|---|---|---|
| `AutoLevel` | `contingency` | MW2 mission loaded at start |
| `Loadout` | `m4_grunt,usp,fraggrenade,flash_grenade` | MW2 weapons given after loading |
| `Capture` | `1` | `0` turns off showing MW2's gun in BL2 (use if MW2 crashes) |

`install.bat` rewrites `bw2.ini`, so add your changes again after reinstalling.

## Troubleshooting

- **F7 doesn't start.** A BL2 message box says why. Usually MW2 hasn't finished loading Contingency yet. If
  MW2's player was outside its level, MW2 reloads the mission by itself; press F7 again after about 30
  seconds.
- **MW2 crashes.** Set `Capture=0` in `bw2.ini` and tell us what you were doing. Crash details are in
  `logs\mw_guest.log` and in Windows Event Viewer → Application.
- **"in use by Borderlands 2" during install.** Quit BL2 and run `install.bat` again.
- **Logs:**

  | Game | Log file |
  |---|---|
  | MW2 | `logs\mw_guest.log` |
  | BL2 | `logs\bl_host.log` |

## Uninstall

Quit both games and run `uninstall.bat`. It removes:
- MW2's `winmm.dll` and `bw2.ini`;
- BL2's `sdk_mods\bw2` folder.

Your saves are not touched. The backup from step 2 of Install is in `backups\`.
