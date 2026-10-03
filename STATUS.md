# Status and roadmap

Last updated 2026-10-03. How to install and play: [INSTALL.md](INSTALL.md). Development log with every finding:
[PROGRESS.md](PROGRESS.md).

## Done

Tested in game (Windows 11, Steam versions of both games):

| Area | What works |
|---|---|
| Link | MW2 and BL2 share state through shared memory; both sides recover from the other game restarting. |
| Movement | MW2 movement on BL2's collision: walk, sprint, jump, crouch, prone, stairs, slopes. Smooth: BL2 dead-reckons MW2's 20 Hz position and blends corrections. |
| View | MW2's arms, gun, crosshair and ammo counter over BL2 (MW2 renders on magenta; keyed, snow filtered). BL2's own gun and HUD are hidden. |
| Gunplay | MW2 bullets damage BL2 enemies with your equipped BL2 gun's damage (headshots x2), found by a world trace plus a check against each enemy's collision cylinder, so invisible triggers and waypoints never block shots. MW2 frags throw BL2 grenades. Weapon switching, reloading, unlimited MW2 ammo, knife. |
| HUD | Shield and health bars, red damage flash; on-screen BL2 messages explain why F7 can't start or why control was handed back. |
| Controls | F7 switches MW2 control on and off; F8 third person (crouch and prone shown as BL2's crouch). Hold X to use stations, doors, vending machines and chests. |
| Setup | MW2 loads Contingency and gives the M4A1 and USP by itself. One-step install and uninstall. |
| Safety | MW2's player can't get lost under its level (rescue, reload, F7 checks). Crash guards for the Steam overlay's graphics hooks in both games. |

Built and covered by the offline tests, but not yet confirmed in game:

- Getting into a vehicle hands control to BL2, and MW2 control comes back by itself when you get out.
- Going down (Fight For Your Life) hands control back to BL2.
- The health bar blinks when health is low.

## Known issues

- **Third person shows your BL2 character, not an MW2 soldier.** MW2 single-player has no third-person body
  for the player (see the roadmap).
- **Jumping isn't shown in third person.** Running and crouching are.
- **Only one MW2 build is supported** (64-bit Steam `iw4sp.exe`, TimeDateStamp `0x6aaaa257`). A Steam update
  means re-finding the addresses (see [CONTRIBUTING.md](CONTRIBUTING.md)).
- **Controller only.** MW2's input comes from XInput; keyboard and mouse aren't mapped.
- **MW2's level is fixed.** Contingency is the default (`AutoLevel` in `bw2.ini`). Other missions should
  work but haven't been tested.

## Roadmap

Good places to start:

1. **Confirm the "not yet confirmed" items above** in game and report the results.
2. **Jump and fall poses in third person.** Drive BL2's animation from MW2's on-ground flag without letting
   BL2's physics move the character.
3. **Health and shield numbers** next to the bars (`src/bl_overlay/overlay.cpp`, `DrawHud`).
4. **Keyboard and mouse support** for MW2's input.
5. **Loadout and mission presets** in `bw2.ini` (`Loadout`, `AutoLevel`), tested and documented.

Bigger pieces:

6. **An MW2 soldier in third person.** Options looked at so far:
   - Borrow one of MW2's own soldiers in the level and pin it to the player. Quick to try, but MW2's AI
     animates it, not your input.
   - Read and draw an MW2 soldier model ourselves. Most faithful, but a large job.

   Either way it needs depth matching, so BL2's walls can hide the soldier.
7. **BL2 present hook without vtable patching**, like the MW2 side (hook the game's own present call). The
   current vtable hook is guarded against the Steam overlay loop, so this is about robustness, not a known
   crash.
8. **Support other MW2 builds** by finding addresses with signatures instead of fixed offsets.
