# Phase 0: recon and GO/NO-GO

2026-10-02. Static analysis of your own installs; neither game was run. RVAs are for
**iw4sp.exe, Steam build of 2026-09-16** (TimeDateStamp `0x6aaaa257`, 4,480,056 bytes). Tools:
`tools/re/pe64.py` (strings, xrefs, `.pdata` function bounds, capstone disassembly), `cmrefs.py`,
`callgraph.py`, `dvarptr.py`, `regioncalls.py`, `handlers.py`.

## Verdict: GO (main plan, MW2 movement on BL2 collision)

| Question | Answer |
|---|---|
| Does MW2 SP route player movement collision through a hookable trace layer? | **Yes.** Every Pmove collision query goes through `pmoveHandlers[h].trace`, a function pointer in a **writable `.data` table** at `exe+0x3bd440`. One pointer swap per handler redirects all of the player's movement collision and nothing else (AI, bullets and physics call the trace functions directly). This is MW2's `BlockCollisionsMixin`. |
| Do bullet traces go through a hookable layer? | Yes (`exe+0x236230` combined world+entity trace, `exe+0x1f8140` world box trace), but the plan doesn't need it: M4 re-casts the shot in BL2's world (BL2 owns hit detection, damage, crits and loot). |
| Can BL2's collision near the player be exported? | **Yes, by the proven route.** BL2's own `Actor.Trace` is callable from the installed Python SDK, so we scan a grid around the player (SkyCraft Stage A / ArkWeb tile scans). Throughput gets measured in Phase 0b (`bench`). A native PhysX 2.8 export (`PhysXCore.dll`) is the later upgrade if scans miss thin geometry. |
| Can both games run at once? | Very likely; to be confirmed in Phase 0b. They are separate processes and Steam allows two games at once. MW2 has a `cl_paused` "Pause the client" path and imports `GetForegroundWindow`/`GetActiveWindow`/`SetCursorPos`, so it will want fake focus in the background. The probe logs exactly what it does. |

Static evidence is complete. Phase 0b (one short test session, read-only probes) confirms the
runtime facts: trace call rates, the `trace_t` layout, BL2 units and trace speed, and focus behaviour.

## Corrections to the plan

1. **MW2 is now x64.** `iw4sp.exe` was rebuilt as PE32+ (`machine 0x8664`, image base
   `0x140000000`) on 2026-09-16, with `steam_api64.dll` and `bink2w64.dll`. The renderer is still
   **D3D9** (`d3d9.dll` import) and it is still the IW4 engine (all the classic dvars are there).
   Consequences:
   - The guest DLL is **x64** and the BL2 host side stays **x86**. They share only memory, so that's fine.
   - Every IW4/IW4x 32-bit address and offset is useless; everything here was found fresh.
2. **SteamStub DRM** wraps the exe (entry point in a `.bind` section), but `.text` and `.data` are
   **not encrypted on disk** (`.text` compresses to 53%, normal for code). Static analysis works and
   a proxy DLL loads normally. No Arxan or anti-tamper sections; SP has no anti-cheat.
3. **MW2 PC still has no controller support.** No XInput import and no XInput strings; only leftover
   `gpadButtonsConfig`/`gpadSticksConfig` dvars. Mouse is `GetCursorPos`/`SetCursorPos` recentring
   and keys are window messages. **Plan:** the guest reads your controller itself (XInput works
   without focus) and writes MW2's **usercmd** directly (`CL_GetUserCmd` @ `0xca300` anchors the
   command buffer). That gives true analog movement (usercmd `forwardmove`/`rightmove`) and
   stick-driven view angles, without faking a mouse. BL2 is told to ignore the controller while
   it drives the puppet.

## MW2 (guest) findings

| What | Where (RVA) | How it was found |
|---|---|---|
| `clipMap_t cm` (collision world) | `0x131f800` | `CM_LoadMap` (`0x1f0220`, string `"CM_LoadMap: NULL name"`) ends with `*checksum = cm.checksum` (`cm+0x190`) and allocates per-partition thread data from `cm.partitionCount` (`cm+0xe0`). Both match IW4's `clipMap_t` with 8-byte pointers. |
| CM module (all collision code) | `0x1ec000-0x1fb000` | 50+ functions reading `cm` fields (`cmrefs.py`) |
| World box trace (`CM_BoxTrace`-like) | `0x1f8140` | called by the game and cgame traces |
| Game trace (world + entities) | `0x236230`, wrappers `0x235d60` (13 callers), `0x236440` | callers of the cmodel trace `0x1f3d00` |
| **`pmoveHandlers[]`** | `0x3bd440` (`.data`, writable) | the only table indirect-called from Pmove (`handlers.py`) |
| handler 0 `.trace` (cgame prediction) | `0xc8600` → `0xc8630` → `0x1f8140` | `{trace, debugLine}` per handler, stride 0x10 |
| handler 1/2 `.trace` (game) | `0x175280` → `0x236230` | |
| slot `.debugLine` | `0x95580` → `0xcd6c0` | `"Client Debug Lines"`; not collision |
| Pmove / BG code | `0x70000-0x80000` | readers of `jump_height`, `player_sprint*`, `g_gravity`, `mantle_*`, `bg_fallDamage*` dvars |
| `Cbuf_AddText(int, const char*)` | `0x1ee890` | `Cbuf_AddText(0, va("devmap %s"))`; takes critical section 0x24, so it is thread-safe |
| `CL_GetUserCmd` | `0xca300` | string `"CL_GetUserCmd: %i >= %i"` |

**Trace signature** (Pmove call site `0x7210b`):
`void trace(trace_t* results, const float* start, const float* end, const Bounds* bounds, int passEntityNum, int contentMask)`.
`Bounds = { float mid[3]; float half[3]; }` (read at `+0xc/+0x10` in `0xc8630`). Masks seen: `0x820015`
(player solid). Results start with `float fraction; float normal[3];`. The rest of `trace_t` is
sampled raw by the probe.

**Useful dvars present:** `cg_drawGun`, `cg_draw2D`, `cg_drawHUD`, `cg_drawCrosshair`, `cg_fov`,
`cg_gun_x/y/z`, `r_clear`, `r_clearColor`, `r_znear_depthhack` (the viewmodel has its own depth
range, which makes cutting it out of the frame straightforward in M3), `r_fullbright`, `r_drawSun`,
`r_drawWater`, `r_drawDecals`, `r_lockPvs`, `r_skipPvs`, `jump_height`, `g_gravity`, `g_speed`,
`player_sprint*`, `sv_cheats`, `developer`, `monkeytoy` (console unlock), `thereisacow`.

**Proxy DLL:** iw4sp.exe imports `WINMM.dll` (`timeGetTime`, `timeBeginPeriod`, `timeEndPeriod`),
which isn't a KnownDLL. A `winmm.dll` x64 proxy next to the exe works (ArkWeb's route).

## BL2 (host) findings

- `Borderlands2.exe` x86, UE3, D3D9 (`d3d9.dll`, `d3dx9_43.dll`), PhysX 2.8 (`PhysXCore.dll`,
  `NxCharacter.dll`), XInput 1.3 + DirectInput 8.
- **Python SDK v3.8 is installed** (`Plugins\pyunrealsdk.dll`, Python 3.14 32-bit, `mods_base`).
  Its `ddraw.dll` is the **plugin loader**: it `LoadLibrary`s DLLs in `Binaries\Win32\Plugins`.
  So, as the hard rules require:
  - **SDK mod (`sdk_mods\bw2`)**: puppet the pawn (`SetLocation`/`SetRotation`, physics, hide),
    camera, damage (`TakeDamage` on the hit enemy), player health, HUD toggles, collision scans
    (`Actor.Trace`), save safety. It can read the shared memory with Python's built-in `mmap`.
  - **Native plugin (`Plugins\bw2_host.dll`, x86)**: only for what the SDK can't do, which is
    drawing MW2's captured viewmodel into BL2's D3D9 frame (M3). It is loaded by the SDK's own
    loader, so **BL2 needs no proxy DLL**.
- Earlier-project note: `sdk_mods\mw2ops.sdkmod` (MW2Ops) is still installed and active. It doesn't
  conflict, but disable it in the SDK mod menu for clean tests.
- The decompiled gamedb (`A:\Boderlands-2-Decomp\...\decompiled`) is unnamed Ghidra output. UE3
  reflection through the SDK is the better map of BL2; the gamedb stays a read-only fallback.

## What SkyCraft and ArkWeb taught us (and what we reuse)

| Idea | SkyCraft / ValCraft | ArkWeb | Border Warfare 2 |
|---|---|---|---|
| Split | Guest (MC) owns player physics, inventory and combat math; host (Skyrim) owns world, NPCs and saves. "If we re-implement a mechanic, the design has gone wrong." | Same. The guest (Spider-Man) owns traversal, the host (Arkham) owns world and combat | MW2 owns movement, aim, ADS, recoil, fire and reload timing. BL2 owns world, enemies, damage, health, loot and saves |
| Collision redirect | Mixin on `BlockCollisions` appends host shapes to MC's block query; MC's step-up, gravity etc. run unchanged | Hook the 3 `hknpWorld` query entry points, filtered to the main world; stream Gotham as tiles | Swap `pmoveHandlers[h].trace` and answer from a BL2 collision field (triangles/boxes streamed from BL2 scans). MW2's own Pmove (step, slide, mantle, jump, sprint) runs unchanged |
| Data source | Stage A: Havok ray grid around the player; Stage C: real shapes | Arkham's own traces in 50 m tiles plus PhysX statics | Stage A: BL2 `Actor.Trace` grid from the SDK (multi-hit columns plus horizontal probes); later PhysX/kDOP export |
| Keep the guest's own world out of reach | (void world) | Gotham built 2 km above New York | The MW2 player stands in a quiet SP map; when the hook is live, MW2's world is *replaced* (not merged) for pmove traces, so MW2 geometry can't block you |
| Link | `Local\SkyCraft_v1`: header + seqlock slots + SPSC rings, named events, heartbeats | `Local\ArkWeb_v1`: same, fixed offsets, one header shared by C++ and Python (`arkweb_proto.py` + layout test) | `Local\BW2_v1`, same design: header + heartbeats, seqlock slots (host, guest, pad, capture), SPSC rings (events, collision), C++ header mirrored in `bw2_proto.py` with a layout test |
| Injection | SKSE plugin / Fabric mod | `winmm.dll` proxy (guest) and `dinput8.dll` proxy (host) with signature-checked hooks; refuse to hook unknown builds | `winmm.dll` x64 proxy in MW2 (build-checked by TimeDateStamp); BL2 through the SDK and its plugin loader |
| Hidden guest | MC window hidden, told it's focused; input forwarded | Fake focus: hook `GetForegroundWindow`/`GetFocus`, swallow `WM_ACTIVATEAPP`/`WM_KILLFOCUS`; virtual XInput pad | Same fake focus plus cursor hooks (MW2 recentres the cursor); controller read in the guest and written as usercmds |
| Frame | GPU-shared layers; CPU fallback first | D3D12 capture: depth-keyed cut-out of the hero, shared D3D11 textures, drawn by the host | D3D9 → D3D9 can't share across processes (BL2's device isn't D3D9Ex), so **CPU path**: MW2 renders the viewmodel over a key colour / depth cut, read back, copied through shared memory, uploaded to a BL2 texture and drawn before BL2's HUD |
| Debugging | Fakes: `fake_skyrim.py`, `fake_guest.py` | `fake_arkham.py`, `fake_spidey.py`; live command files `logs\*_cmd.txt`; read-only recon DLLs first | `tools\fake_bl2.py`, `tools\fake_mw2.py`; `logs\mw_cmd.txt` / `logs\bl_cmd.txt`; Phase 0b probes (this folder) |
| Save safety | Snapshots, safe states on link loss | Host refuses to move Batman far under the streets; `tp` command | Puppet is moved only to positions validated by a BL2 trace (ground found, inside the level); on link loss the pawn is restored to normal control at its last valid spot |

Licences: SkyCraft and ValCraft are MIT, and any code taken from them keeps its notice
(`THIRD-PARTY-NOTICES.md`). **ArkWeb has no licence file**, so it is used for design only; no
ArkWeb code is copied. `src/common/*` is our own implementation of the same patterns.

## Phase 0b: runtime confirmation (probes built and installed)

- `recon_dll/mw_recon` → `winmm.dll` in MW2. Read-only. It wraps the three `pmoveHandlers` trace
  pointers with counters that call the originals unchanged, samples raw `trace_t` results, logs
  focus messages and offers an opt-in fake focus (`focus on`). Log `logs\mw_recon.log`; commands
  `logs\mw_cmd.txt` (`status`, `focus on|off`, `sample <n>`, `con <console cmd>`).
- `sdk_mods/bw2_recon` → BL2 SDK mod. Read-only. Logs pawn location, rotation, cylinder, eye
  height, speeds and camera POV/FOV every 5 s. `bench <n> [extent]` times `Actor.Trace` from Python.
  Log `logs\bl_recon.log`; commands `logs\bl_cmd.txt` (`status`, `bench`, `con`, `py`).

Questions it closes: trace rate per side while walking/jumping/mantling; whether MW2 stops
simulating when unfocused and whether fake focus fixes it; `trace_t` field layout; BL2 units
(cylinder size) and how many traces per frame the SDK can afford for collision scans.

## Phase 0b results (2026-10-02 22:30-22:38, Sanctuary + MW2 "Team Player" / roadkill)

**MW2 (`logs/mw_recon.log`)**
- All three `pmoveHandlers` trace pointers hooked cleanly on the known build; the game ran normally.
- Rates on foot: **cgame ~4,500-5,600 traces/s** (prediction replays Pmove every frame), **game ~520-590/s**,
  handler 2 only for a vehicle/other entity (`pass=1279`, 6-unit cube). Player bounds: `mid (0,0,35) half (15,15,35)`
  standing, `mid (0,0,39) half (9,9,31)` for a stance probe. Masks: `0x281c011` (player clip), `0x820015`,
  `0x1000000` (thin 0.1-unit probe).
- **Background:** after `WM_ACTIVATEAPP 0` (22:33:16) Pmove kept running at full rate. MW2 SP does
  **not** pause when unfocused. Input is focus-gated (`GetForegroundWindow` is polled at the same rate as
  `GetCursorPos`), which doesn't matter because the guest writes usercmds itself.
- `trace_t` (x64), decoded from hits on the ground:

  | Off | Field | Example |
  |---|---|---|
  | 0x00 | float fraction | 0.1406 |
  | 0x04 | float normal[3] | (0.058 0.155 0.986) |
  | 0x10 | int surfaceFlags | 0x00a00000 |
  | 0x14 | int contents | 1 |
  | 0x18 | Material* | pointer |
  | 0x20 | int hitType | 1 (entity; world counts as entity) |
  | 0x24 | u16 hitId | 0x7fe = ENTITYNUM_WORLD (2046) |
  | 0x26/0x28/0x2a | u16 modelIndex / partName / partGroup | 0 |
  | 0x2c / 0x2d / 0x2e | bool allsolid / startsolid / walkable | 0 / 0 / 1 |
- Probe bug, harmless: the window it picked isn't the foreground one (all top-level windows get
  `WM_ACTIVATEAPP`). M1 tracks focus from messages instead.

**BL2 (`logs/bl_recon.log`)**
- Player: `WillowPlayerPawn`, cylinder **r 42, half-height 81**, eye 71 above the centre (152 above the
  feet), GroundSpeed 440, JumpZ 630. Rotation is in UE units (65536 = 360°).
- **Units:** the MW2 player is 70 tall with the eye at 60, BL2's is 162 / 152. **1 MW2 unit = 1 inch =
  2.54 BL2 units (1 UU ≈ 1 cm).** That is the scale for the coordinate conversion.
- **SDK gotcha (solved):** `Actor.Trace` and `FastTrace` return defaults when called from Python,
  because UE3 `ProcessEvent` skips functions with a numbered native index (`Trace` = 277,
  `FastTrace` = 548; `iNative` is a u16 at `UFunction+0x90`). Clearing `iNative` for the duration of
  the call fixes it (`TraceComponent`, which has no index, always worked).
- **Trace throughput from Python: 64 µs per line trace, 81 µs per 20-unit box trace**, 400/400 hits on a
  20x20 grid at 100-unit spacing: Terrain, StaticMeshCollectionActor, StaticMeshActor, InterpActor
  (moving platforms/doors) and WillowInteractiveObject. At about 2 ms per frame that is ~1,800 traces/s,
  enough to rescan a 16 m x 16 m patch at 50-unit resolution about once a second. A native call path
  (host DLL) is the upgrade if needed.

**Phase 0 closed: GO confirmed at runtime.**
