# Border Warfare 2: progress

Goal: playable MW2 (2009) gunplay inside Borderlands 2. You see BL2's world with MW2's hands and gun,
move/aim/shoot/reload with MW2 feel, and your shots damage BL2 enemies.

## Status

| Milestone | State |
|---|---|
| M0 Recon + GO/NO-GO | **Done, GO** (static + Phase 0b runtime; `recon/PHASE0.md`) |
| M1 Link + coords + fakes | **Done** (puppet follows fake MW2; controller drives MW2 in background) |
| M2 Movement on BL2 collision | **Done**: walk, sprint, look, jump, crouch, prone, stairs confirmed in game |
| M3 Viewmodel capture into BL2 | **Built** (MW2 Present capture + magenta key, BL2 Present overlay, BL2 arms/gun hidden); in-game test next |
| M4 Gunplay → BL2 damage | **Done**: MW2 shots kill BL2 enemies (health 35 -> 0 logged), grenades thrown |
| M5 Playable pass | |

## What works
- **Link** `Local\BW2_v1` (`protocol/bw2_protocol.h`, `src/common/link.h`, `tools/bw2_proto.py`): header +
  heartbeats, seqlock slots (HostState, PadState, GuestState), SPSC rings (events, commands, collision).
  `tests/layout_test` (x86 + x64) and `python tools/bw2_proto.py` check the layout: 47/47 fields match.
- **Coordinates** (`tools/bw2_coords.py`): anchor-based MW2 inches ↔ BL2 uu (×2.54, Y mirrored,
  yaw/pitch conventions), with a self-test.
- **MW2 guest** (`src/mw_guest`, winmm.dll x64): hooks installed from MW2's main thread (through our own
  `timeGetTime` export), publishes the player every client frame (origin, velocity, view angles, stance,
  usercmd), reads the controller (XInput) or the virtual pad, writes analog move/look into the usercmd
  and view angles, and sends buttons as MW2 `+command` binds. Supports teleport (`setviewpos`) and live
  commands (`logs\mw_cmd.txt` or the ring): `status`, `pad`, `con`, `tp`, `calib`, `publish on|off`.
- **BL2 host mod** (`sdk_mods/bw2`): heartbeat + HostState, `drive on|off` puppets the pawn from MW2
  (anchored, snapped to BL2 ground, refuses unsafe steps, blocks BL2 fire/jump/reload while driving),
  `home`, `mw <cmd>`, `bench`, `con`, `py`. Fix for calling numbered natives from Python (`natives.py`).
- **Fakes**: `tools/fake_mw2.py` (scripted player), `tools/fake_bl2.py` (monitor / scripted pad test with
  PASS/FAIL / send). `tests/test_bl_mod.py` runs the BL2 mod against SDK stand-ins + fake_mw2: OK.

## Controller map (MW2 console defaults)
LS move, LS click sprint (latched), RS look, RS click knife, LT aim, RT fire, A jump, B tap crouch /
hold prone, X reload/use, Y switch weapon, RB grenade, LB tactical, D-pad action slots. Start/Back stay with BL2.

## M1 in-game results (2026-10-02)
- BL2 puppet followed fake_mw2's square: 1036 moves applied, 147 refused by BL2 collision, on BL2 ground, yaw followed. PASS.
- Virtual pad -> real MW2 in the background: look/turn PASS. Buttons FAILED because in this build `+attack`,
  `+gostand`, ... are bind-only table entries (`exe+0x3be770`), not console commands, so Cbuf ignored them.
  Fixed: the guest now ORs CL_CmdButtons' bits into the usercmd (attack 0x1, sprint 0x2, melee 0x4, use/reload 0x20,
  up 0x400, ADS 0x800, breath 0x2000, frag 0x4000, smoke 0x8000) and writes the client stance field (+0x1c).
- View angles are now published from playerState+0x108 (cmd angles + delta_angles at ps+0x60), which is
  what MW2 actually moves and looks with.
- No XInput controller was detected on the PC during the test.

## M2 (built 2026-10-02)
- `sdk_mods/bw2/scan.py`: BL2 scanned in 768 uu tiles of 32 uu columns (down multi-hit tops, up undersides,
  knee/chest wall probes) -> merged boxes -> guest space. ~4.7k traces per tile. `tests/test_scan.py` (13 checks).
- `src/common/field.h`: guest-side box field (immutable snapshot, 64 in buckets) with Quake's brush clip and
  SURFACE_CLIP_EPSILON; uncovered space answers "blocked" (hold). `tests/field_test.cpp` (10 checks),
  `tests/walk_test.cpp` (scan -> field -> step/slide walker: ground, 17 deg hill, wall, door, table).
- Guest: with HostState kHostCollision, every Pmove trace is answered from the field (MW2's world is out of the
  picture); `trace_t` filled from a template of a real MW2 hit. Teleports write the game playerState.
- Host: `drive on [dz]` = scanning (anchor MW2 player 12000 in below its level) -> arriving (collision on, teleport)
  -> driving (pawn + eye exactly at MW2's); tiles stream in a 3x3 around the pawn, rescanned every 30 s.
  `tests/test_bl_mod.py` runs the whole flow against SDK stand-ins + fake_mw2: OK.

## M2 in-game findings (2026-10-03)
- UE3 convex/BSP collision reports a hit at the trace START when the start is inside it; the scanner now
  probes down through solids in 64 uu steps and finds undersides with an up trace (was: all solid; then a
  doubling search that skipped the space under a Sanctuary arch).
- Python traces cost 32-64 us each; native scanner `src/bl_scan/scan.cpp` (bw2_scan.dll, x86, via the SDK's
  exported `process_event`) is box-for-box identical to scan.py and scans 9 tiles in ~2 s instead of ~17 s.
- MW2's player settling into the floor froze Pmove ("all solid"): guest lifts it out (field::LiftOut).
- MW2's player died from fall damage -> checkpoint reload -> out of BL2's world. Guest now enables
  sv_cheats (both dvars, value at dvar+0x10) and sends god, notarget, no fall damage when collision starts.
  Host stops driving if MW2's player jumps > 3000 uu or BL2's pawn changes.
- Sprint = kbutton at exe+0x52fdc4 (CL_KeyMove 0xcf900 sets usercmd bit 0x2 from it); bit 0x2000 is hold breath.

## M4 (built 2026-10-03)
- Guest hooks MW2's Bullet_Fire (0x15a940, numbers each trigger pull) and per-bullet trace (0x158d90: start/end
  at +0x1c/+0x28 of arg 3, shooter entity u16 at +0x7e of arg 2); the player's bullets go to the event ring
  as ShotEvent. RB released -> kRecGrenade.
- Host (`sdk_mods/bw2/gunplay.py`): each bullet traced through BL2 (actors + bones) and handed to the equipped
  BL2 gun's ProcessInstantHit -> BL2 damage, elements, crits, kills, loot. MW2 pellets matched to the BL2 gun's
  ProjectilesPerShot. MW2 frag -> BL2 StartThrowGrenadeSkill (ThrowGrenade let through for 2 s).

## M4 in-game findings (2026-10-03)
- The first hooks (0x15a940/0x158d90) were a debug-only bullet path; the real one is Bullet_Fire 0x159160
  (gentity*, float spread, weaponParms*, gentity*, int) -> Bullet_Trace 0x15ac60 (bp: shooter +4, start +0x1c, end +0x28).
- BL2's ProcessInstantHit from outside its firing sequence registers hits but applies NO damage; the host now
  calls the hit enemy's TakeDamage (keyword args: Damage, InstigatedBy=pc, HitLocation, Momentum, DamageType,
  HitInfo, DamageCauser=weapon) with the BL2 gun's InstantHitDamage (x2 on head bones).
- A broad `find_all(WillowAIPawn)` + GetHealth sweep from the py command crashed BL2: never sweep pawns.
- MW2 automation: guest loads `spdevmap contingency` at the main menu (devmap is ignored in SP), then cheats,
  god, notarget, no fall damage, loadout m4_grunt,usp,fraggrenade,flash_grenade (bw2.ini AutoLevel/Loadout).

## M3 (built 2026-10-03)
- Seen in MW2 (screenshots): in the void with r_clear 3, r_clearColor 1 0 1 1 and r_skipPvs 1 (no world/sky)
  the frame is just hands + gun (+ MW2 HUD) on magenta.
- Guest `src/mw_guest/capture.h`: d3d9 vtable Present/Reset hook, back buffer -> sysmem, magenta -> alpha
  (soft edge + despill), two slots in `Local\BW2_frame_v1` (FrameHeader), <= 75 fps, only while the host asks.
- Host `src/bl_overlay/overlay.cpp` (bw2_overlay.dll, x86, ctypes): Present hook draws the newest frame with
  alpha blending (state block saved/restored). `sdk_mods/bw2/overlay.py` hides BL2's Arms and
  Weapon.FirstPersonMesh while driving; BL2's HUD stays.

## Next
1. M2 in-game test: `drive on` in Sanctuary, walk/sprint/jump/crouch on BL2 geometry with the controller.
2. M3: capture MW2's viewmodel and draw it in BL2; hide BL2's gun and HUD.

## Known issues / risks
- All MW2 addresses are pinned to iw4sp.exe TimeDateStamp `0x6aaaa257`.
- M1 puppet uses BL2 ground under MW2's XY (MW2's own geometry still drives MW2 heights until M2).
- `kGuestOnGround` is approximated from vertical speed (playerState ground entity offset not pinned yet).
- D3D9 → D3D9 frame sharing has to go through the CPU (BL2's device isn't D3D9Ex).
- Disable the old `mw2ops.sdkmod` for clean tests.

## Environment
- MSVC 14.44 Build Tools 2022 (x86 + x64), Windows SDK 10.0.26100. `build.bat`.
- Python 3.12 per-user (tools/fakes/tests), with capstone, pefile, numpy.
- BL2 Python SDK v3.8 (Python 3.14 32-bit) in the game.

- 2026-10-03: MW2 crashed at startup after the M3 capture went in (log stopped before any "capture:" line). Cause:
  throwaway D3D9 devices were created on the worker thread while MW2 made its own, and vtables were patched
  PAGE_READWRITE. Fix: the guest no longer creates throwaway devices; it adopts MW2's device only after 100+
  gameplay frames (every 3 s, max 20 tries), patches with PAGE_EXECUTE_READWRITE, and logs each step.
  bw2.ini `Capture=0` turns frame sharing off completely.
- 2026-10-03 (2): MW2 still crashed at the first adopt try (the sweep of MW2's .data, with QueryInterface on
  whatever looked like a d3d9 object). Replaced by MW2's own device global: IDirect3D9* at exe+0x410fd18
  (written after Direct3DCreate9, exe+0x10e26), device at exe+0x410fd20 (CreateDevice, exe+0x10348, flags 0x46).
  Frame slot indices are masked to 0/1.
- 2026-10-03 (3): M3 WORKS in game. MW2 device hooked (no crash), 1920x1080 captured, BL2 draws it (overlay drawn
  876 / uploaded 265 in 4 s, 0 errors). Screenshot: MW2's M4A1 + arms keyed cleanly over BL2's world, MW2
  crosshair. Remaining M3 polish: BL2 HUD hiding, feel/latency check by the user.
- 2026-10-03 (4): User: "it looks good". BL2's HUD now goes off while driving (ToggleHUD when myHUD.bShowHUD is
  wrong, back on when driving stops); MW2's HUD (ammo, grenades) stays. Fixed overlay.report() passing a 4-value
  buffer to bw2_overlay_stats, which writes 8. BL2 health display returns in M5 (god mode covers tests for now).
- 2026-10-03 (5): In-game check passed: BL2 HUD hides while driving and is back after drive off (bShowHUD=True); kills with the MW2 view. User: "a bit janky". Next: M5.
- 2026-10-03 (6): User: look/world/movement fine; "some enemies die, some dont". Log: 141 shots, 0 misses, logged enemy hits all killed (one shot, e.g. 934 -> 0). Added per-class hit tallies + survivor logging to find the cause.
- 2026-10-03 (7): Test: shooting dead until the user knifed/moved; log: bullets hit a WillowTrigger at 0 uu (the
  spawn trigger around the puppet). Fix: gunplay re-traces past non-blocking actors (Trigger/Waypoint/Volume,
  bBlockActors false); test stub covers it. MW2 crashed: 0xc00000fd stack overflow in gameoverlayrenderer64.dll
  right after a device reset = Present hook fight with the Steam overlay. Fix: guest no longer patches D3D
  vtables; MW2's two direct calls to its present function exe+0x45060 (at exe+0xc77d, exe+0x45757; arg bit 2 =
  present) are re-pointed through a near stub to PresentHook, which captures from the device global first.
  Capture now copies the back buffer straight to a SYSTEMMEM surface (no DEFAULT-pool surface, no Reset hook).
  Guest also sends the player home if the host dies mid-drive (no more endless void fall).
  Open: weapon switch (Y -> Cbuf "weapnext") reported not working; BL2 overlay still uses vtable hooks (same
  Steam-overlay risk on a BL2 device reset).
  Also: guest treated a torn HostState SeqRead as "host gone" (133 collision off/on flips in 5 min); now a torn read keeps the last state and "gone" needs 3 s without a heartbeat.
- 2026-10-03 (8): In game: shooting works from spawn (61 enemy hits, 0 survivors, 176 trigger pass-throughs), no crash, collision flips gone. Y weapon switch: weapnext/weapprev are bind-only key actions (table exe+0x3be770, dispatcher exe+0xce450); guest now calls the weapon cycle exe+0xc2d90(lc, 1) from CL_CreateCmd; guest command "weapnext" for tests. Installed, untested.
- 2026-10-03 (9): Y weapon switch confirmed in game. M5 built: F7 keybind (mods_base keybind, rebindable) toggles
  MW2 control; overlay DLL draws BL2 shield/health bars + red edge flash on damage (bw2_overlay_hud); god mode
  now off by default (`bw2 god on` for tests); going down (IsInjured / health 0) hands control back to BL2;
  WillowPopulationPoint passes bullets. INSTALL.md written. install.bat: `bl` mode (BL2 only), deletes the old
  bw2_overlay_[tpa].dll test files. Needs BL2 restart to load the new overlay DLL; untested in game.
  Known risk: BL2 overlay still patches D3D vtables (Steam-overlay hook fight possible on a BL2 device reset).
- 2026-10-03 (10): F7 + bars confirmed in game (screenshot). Bug: a restarted BL2 host has teleportSeq 0, guest took it as a teleport to (0,0,0) -> void. Fixed (seq 0 only resyncs the ack); built, installs on next MW2 close. STATUS.md added (done / left).
- 2026-10-03 (11): In game: red damage flash confirmed. User stuck after F7: MW2's player was at z -97,000,000
  (float precision: every trace solid, no movement; jittery viewmodel). Chain: (0,0,0) teleport bug -> void fall
  -> each F7 anchored dz below the current (void) origin -> MW2 restart applied a stale teleport #3 from the
  running BL2 session. Fixes: guest syncs its teleport ack on every new BL2 session (hostPid change) instead of
  executing the old request; guest rescues the player when below z -30000 while not driving (home, else
  spdevmap reload); home is only recorded above that; host refuses F7 when MW2's player is below the level
  (asks the guest to reload) or airborne. F7 failures and automatic stops now show a BL2 on-screen message box
  (WillowHUDGFxMovie.AddTrainingText, tested). Built; watcher installs each side when its game closes.
- 2026-10-03 (12): User: "it looks good" after a fresh start (F7 anchored at z -11905, guest synced to the new BL2 session). STATUS.md updated.
- 2026-10-03 (13): User: shooting/reloading fine; "world lags a bit". Cause: MW2 (~140 fps) and BL2 (~150 fps)
  frames don't line up, so the puppet reused or skipped MW2 samples. Fix: puppet predicts MW2's position by
  velocity x sample age (GuestState.qpc is QPC; Python's perf_counter is the same clock, checked), capped at 50 ms
  and off above 600 in/s. pawn.Velocity = MW2's velocity so BL2's animation tree runs. F8 third person: BL2
  `camera 3rd` (tested live) + MW2 `cg_drawGun 0` via the guest's `con` command; restored on F8/drive off.
  fake_mw2 now stamps real QPC. Installed (BL2 restart needed); untested in game.
- 2026-10-03 (14): Floating bug (user screenshots: high up, walking on air, slowly sinking). Log: applied 0,
  refused 61097 from the first driving frame (591 uu). The pawn kept moving during the ~2 s scan (user running
  at F7), so every MW2 sample was > MAX_STEP away and refused; the new pawn.Velocity (for animations) was set
  before the refusal and BL2's own physics (never forced back to PHYS_NONE on that path) moved the pawn.
  Fixes: pawn snapped onto the anchor when frozen (arriving), PHYS_NONE enforced before any check, Velocity set
  only after a successful move (zero while refusing), a jump MW2 keeps for 1 s is accepted. Test: pawn moved
  600 uu during the scan -> 0 refused. Third person itself looked right (screenshot). Installed (BL2 restart).
- 2026-10-03 (15): User: levitation fixed, F8 good, collision decent; running "janky, not in sync with the
  world". Cause: the pawn was positioned in WillowGameViewportClient:Tick, which runs after UWorld::Tick has
  already updated the camera, so the world was drawn from last frame's position while the rotation (set in
  UpdateRotation) was current. Fix: puppet() now runs inside the UpdateRotation hook (same frame, before the
  camera); the viewport tick only falls back if the hook didn't run this frame (S.hook_frame). Test drives the
  hook path: exactly one move per frame. Installed (BL2 restart).
- 2026-10-03 (16): User: better, but "the world doesn't keep up with the MW2 character"; out of MW2 ammo.
  World: BL2 presents a game frame later than the MW2 frame overlaid at Present, so the gun is ahead. Added a
  lead (default 20 ms, `bw2 lead <ms>`, max prediction 100 ms) to the position prediction and predicted the
  view too (turn rate from consecutive MW2 samples, capped 720 deg/s). Ammo: `player_sustainAmmo 1` in
  MakeSafe (sent live to the running MW2 too). Tests pass; BL2 side installed (restart), MW2 DLL installs on close.
- 2026-10-03 (17): User: lead + view prediction felt worse (looking/moving laggy). Switched off live (lead 0, predicted_view patched out), then removed from code: view uses MW2 angles as sampled, lead default 0 (`bw2 lead <ms>` kept for experiments). Installed.
- 2026-10-03 (18): User: "moves forward, micro lags back, rubber bands forward" (video in project folder; no
  ffmpeg here to view it). tools/jitter_probe.py (read-only, ~1 kHz) over 90 s: MW2's published position 0
  backward steps; BL2 pawn 2147 backward steps (worst 15.8 in). MW2's ps origin only changes every ~30-40 ms
  (7-13 in steps) while GuestState is published every frame, so predicting by publish age made a sawtooth.
  Fix: dead reckoning from the moment the origin changed + correction blended over 80 ms (BLEND_S); simulated on
  the recording: 0 backward steps, speed sd 195 -> 66 in/s. Installed (BL2 restart).
- 2026-10-03 (19): Still some rubber banding when running + turning. Second recording (with yaw): BL2 pawn now
  smooth (81 backward steps, worst 0.33 in; yaw never reverses), but MW2's *published* position stepped back 46
  times (worst 5.2 in) and only changed every ~50 ms. Direct read of both playerStates while running: cgame
  prediction ps changes every ~3 ms, game ps every ~49 ms. The guest read the cgame ps at CL_CreateCmd, when it
  still held the server copy (prediction refills it from the snapshot, then replays). Fix: PmoveHook records
  origin/velocity right after each cgame prediction Pmove; GuestState publishes that (fallback: ps read).
  Built; installs when MW2 closes.
- 2026-10-03 (20): User: rubber banding again after (19). Recording 4 (tools/jitter_probe.py --wait): MW2's
  position now changes every frame (~10 ms) but MW2 runs ~100 fps with uneven frames (3-23 ms); the host
  stamped each position when it saw it, so implied speed swung 86..1400 in/s on a steady ~230 run. Fix: the
  guest stamps GuestState.qpc with the QPC of the prediction that produced the position; the host dead-reckons
  from that time. Next suspect for MW2's uneven frames: the synchronous GetRenderTargetData in the capture.
  Built, offline test OK; installs as each game closes.
- 2026-10-03 (21): User: (19)+(20) "much worse"; best was right after (18). Reverted both: guest publishes the ps read at CL_CreateCmd again (no post-prediction capture), host dead-reckons from when it saw the change. Kept: tools/jitter_probe.py (--wait). Lesson: the 20 Hz server position + host dead reckoning + 80 ms blend feels better than per-frame cgame positions. Rebuilt; installs as each game closes.
- 2026-10-03 (22): User: smoothness good again; shooting broken (no hits). Log: 68 bullets, 67 misses, 536 pass-throughs = 8 per bullet: the bullet started inside a non-blocking volume, which a trace from inside reports right at the start; 2 uu steps never left it. Fix: the step grows x4 while the hit is at the start (2, 8, 32, 128 ...), 16 tries; pass-through classes counted in the report. Test stub now has a 40 uu volume around each bullet start (11/11 hits). Installed and reloaded live (importlib.reload of bw2.gunplay, bw2.G rebound).
- 2026-10-03 (23): User: shooting works, then 40+ bullets hit nothing. Log: 1750 WillowWaypoint pass-throughs (5 per bullet); the growing step jumped ~700 uu past enemies. New bullet tracing (gunplay._trace): world-only Trace (bTraceActors False: ignores triggers/waypoints/volumes), then ray vs collision cylinder of each pawn in WorldInfo.PawnList (not ours, not players; dead ones skipped); nearest before the wall wins; a short actor trace just in front of it supplies the bone, else a cylinder hit with head = top 22 uu. Test stub: enemy on each bullet line, trigger volume at every bullet start; 11/11. Installed and reloaded live (14 pawns in the list).
- 2026-10-03 (24): User: shooting "much better" with the new tracing. STATUS.md updated.
- 2026-10-03 (25): BL2 crash-proofing. d3d9hook: thread-local re-entry guard in DevPresent/ScPresent/DevReset;
  re-entered = the Steam-overlay hook loop after a device reset -> call d3d9.dll's own function directly (or
  skip that one call if unknown); loops counted (bw2_overlay_guard, shown in `bw2 status`). New
  tests/hook_loop_test.cpp (x86, in build.bat) recreates the loop on a real device: OK. It also showed that
  every device here has its own heap vtable copy: Install's 16 throwaway devices only filled the hook table
  (kMax) with freed vtables. Install now patches nothing and just learns d3d9's Present/Reset/swap-chain
  Present from a throwaway device; the game's device is hooked via AdoptGameDevices as before. Installs when
  BL2 closes; check `bw2 status` -> "hook guard ... d3d9 present known".
- 2026-10-03 (26): Hook guard confirmed live (d3d9 present/reset known, 0 loops). Third-person MW2 soldier:
  no player body / third-person view exists in MW2 SP (only veh_thirdPersonCam; no spawn command) -> user agreed
  to polish first, MW2-soldier feasibility later. Polish: BL2 pawn.bIsCrouched follows MW2 crouch/prone (live
  test: crouch animation, cylinder unchanged; reset on drive off). Snow specks: capture::Despeckle removes
  see-through pixels (alpha < 200) with no solid neighbour; tests/despeckle_test.cpp (in build.bat) keeps
  every pixel of a soft-edged block. Installs as each game closes.
- 2026-10-03 (27): User: snow filter + crouch look good; wants vehicles (Catch-A-Ride station). Added: MW2's X
  held >= 0.35 s (usercmd 0x20) -> BL2 pc.Use() (stations, doors, vending, chests); getting into a vehicle
  (pc.Pawn becomes a *Vehicle*) hands control to BL2 and stop_drive never moves/re-physics a pawn that isn't
  ours (it used to SetLocation/SetPhysics whatever pc.Pawn was); after getting out, MW2 control resumes by
  itself (retried 1/s for 20 s). start_drive now waits until MW2's player finished its last teleport (test
  caught a resume saving a void "home"). Test covers vehicle handover + resume. Installed (BL2 restart).
