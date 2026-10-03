// Border Warfare 2 - MW2 guest (winmm.dll proxy for iw4sp.exe, x64).
//
// M1: joins the shared-memory link, publishes MW2's player (position, velocity, view, stance) every
// client frame, and drives MW2 from your controller even while MW2 is in the background:
//   - analog: left stick -> usercmd forwardmove/rightmove, right stick -> the client's view angles
//   - buttons -> usercmd button bits and the client's stance field (what keyboard binds end up setting)
// The virtual pad in shared memory (PadState, kPadActive) replaces the controller for scripted tests.
// Pmove's collision traces are wrapped and counted (M2 answers them from BL2's world).
//
// Hooks are installed from MW2's main thread (inside our own timeGetTime export, which the exe calls
// every frame), so no thread can be executing a prologue while it's patched.
// Addresses are for iw4sp.exe TimeDateStamp 0x6aaaa257; any other build only logs and forwards.
//
// Log: <LogDir>\mw_guest.log. Commands: <LogDir>\mw_cmd.txt or the link's command ring:
//   status | pad | con <console command> | tp <x> <y> <z> [yaw] | calib | publish on|off
#define BW2_OWN_timeGetTime
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../common/field.h"
#include "../common/fwd_winmm.h"
#include "../common/link.h"
#include "../common/util.h"
#include "capture.h"

#pragma comment(linker, "/export:timeGetTime=bw2_timeGetTime")

using namespace bw2;

namespace
{
	// ---- iw4sp.exe build 0x6aaaa257 (see recon/PHASE0.md) -------------------------------------------
	constexpr uint32_t  kExeStamp = 0x6aaaa257;
	constexpr uintptr_t kPmoveHandlers = 0x3bd440;  // { trace, debugLine } x 3, stride 0x10
	constexpr uintptr_t kCgTrace = 0xc8600, kGTrace = 0x175280;
	constexpr uintptr_t kCm = 0x131f800;            // clipMap_t: name +0, isInUse +8
	constexpr uintptr_t kCbufAddText = 0x1ee890;
	// Weapon cycle (localClient, next): what the key actions "weapnext" (64) / "weapprev" (68) run in the key-action
	// dispatcher exe+0xce450. In this build those are bind-only actions, so Cbuf("weapnext") did nothing.
	constexpr uintptr_t kWeapCycle = 0xc2d90;
	constexpr float     kVoidZ = -30000.0f;  // below this MW2's player has left its level (Contingency's ground is ~100)
	constexpr uint8_t   kWeapCycleSig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83 };
	constexpr uint8_t   kCbufSig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xf9, 0x48, 0x8b, 0xda, 0xb9 };
	constexpr uintptr_t kPmove = 0x7cf40;           // void Pmove(pmove_t*); pm->ps at +0, pm->cmd at +8
	constexpr uint8_t   kPmoveSig[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x40 };
	constexpr uintptr_t kCreateCmd = 0xd03a0;       // usercmd_s* CL_CreateCmd(usercmd_s*, int localClientNum)
	constexpr uint8_t   kCreateCmdSig[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20 };
	constexpr uintptr_t kClientActive = 0x533cc0;   // clientActive_t[], stride 0xce9c
	constexpr uintptr_t kClientStride = 0xce9c;
	constexpr uintptr_t kClStance = 0x1c;           // int: 0 stand, 1 crouch, 2 prone (CL_CreateCmd turns it into buttons)
	constexpr uintptr_t kClViewAngles = 0x54;       // float[3]: pitch (down +), yaw, roll
	// playerState_s
	constexpr uintptr_t kPsPmFlags = 0x0c, kPsOrigin = 0x1c, kPsVelocity = 0x28;
	constexpr uintptr_t kPsDeltaAngles = 0x60, kPsViewAngles = 0x108;  // PM_UpdateViewAngles (0x7bab0)
	constexpr uintptr_t kPmoveFromCgPredict = 0xb26b5;
	constexpr uintptr_t kPmoveFromGame = 0x15c026;       // return address of the game's (server) Pmove call
	// trace_t (x64, Phase 0b): fraction +0, normal +4, surfaceFlags +0x10, contents +0x14, material* +0x18,
	// hitType +0x20, hitId u16 +0x24, allsolid +0x2c, startsolid +0x2d, walkable +0x2e
	constexpr size_t    kTraceBytes = 0x30;
	constexpr uint16_t  kEntityNumWorld = 0x7fe;  // return address of cgame prediction's Pmove call (Phase M1 log)
	// usercmd_s
	constexpr uintptr_t kCmdButtons = 0x04, kCmdForward = 0x1a, kCmdRight = 0x1b;
	constexpr uintptr_t kSprintKbActive = 0x52fdc4;   // kbutton CL_KeyMove (0xcf900) turns into usercmd bit 0x2
	constexpr uintptr_t kSvCheats[2] = { 0xd9c038, 0x19c5ed8 };  // dvar_t* globals (game, server); value byte at +0x10
	constexpr uintptr_t kDvarValue = 0x10;
	// M4: MW2's bullets. Bullet_Fire(gentity*, float spread, weaponParms*, gentity*, int) runs once per bullet
	// (players, AI, turrets; a shotgun calls it per pellet). Its first Bullet_Trace(bp, weaponDef, ?, results,
	// int) has the bullet: start bp+0x1c, end bp+0x28 (MW2's spread applied), shooter entity number bp+4
	// (0 = the player); later Bullet_Trace calls of the same bullet are penetration re-traces.
	constexpr uintptr_t kBulletFire = 0x159160;
	constexpr uint8_t   kBulletFireSig[] = { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x55, 0x48, 0x8d, 0xa8, 0x38, 0xfc, 0xff, 0xff, 0x48, 0x81, 0xec,
		  0xb8, 0x04, 0x00, 0x00 };
	constexpr uintptr_t kBulletTrace = 0x15ac60;
	constexpr uint8_t   kBulletTraceSig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
	constexpr uintptr_t kBpShooter = 0x04, kBpStart = 0x1c, kBpEnd = 0x28;

	using TraceFn = void (*)(void*, const float*, const float*, const float*, int, int);
	using PmoveFn = void (*)(void*);
	using CreateCmdFn = uint8_t* (*)(uint8_t*, int);
	using CbufFn = void (*)(int, const char*);
	using WeapCycleFn = void (*)(int, int);
	using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);

	struct XInputState
	{
		DWORD   packet;
		WORD    buttons;
		BYTE    lt, rt;
		SHORT   lx, ly, rx, ry;
	};

	// ---- state -------------------------------------------------------------------------------------------
	Link                  g_link;
	Ring                  g_cmdRing;
	bool                  g_linked = false;
	bool                  g_buildOk = false;
	CbufFn                g_cbuf = nullptr;
	WeapCycleFn           g_weapCycle = nullptr;
	std::atomic_bool      g_weapReq{ false };  // "weapnext" command: switch on the main thread's next usercmd
	TraceFn               g_traceOrig[3] = {};
	std::atomic<uint32_t> g_traces{ 0 };
	PmoveFn               g_pmoveOrig = nullptr;
	CreateCmdFn           g_createCmdOrig = nullptr;
	std::atomic<uintptr_t> g_psAny{ 0 };  // the player's playerState (last Pmove call)
	std::atomic<uintptr_t> g_psCg{ 0 };   // cgame's predicted playerState: updated every client frame
	uintptr_t             g_pmoveCallers[4] = {};
	std::atomic<DWORD>    g_mainTid{ 0 };
	std::atomic_bool      g_hooksDone{ false };
	HWND                  g_hwnd = nullptr;
	XInputGetStateFn      g_xinput = nullptr;
	int                   g_padIndex = -1;
	DWORD                 g_padRescan = 0;
	LARGE_INTEGER         g_qpcFreq, g_lastQpc;
	uint64_t              g_frame = 0;
	uint32_t              g_teleportAck = 0;
	std::atomic_bool      g_publish{ true };  // `publish off` lets tools/fake_mw2.py stand in while MW2 runs

	// ---- BL2 collision (M2) ----
	Ring                                                 g_colRing;
	Ring                                                 g_eventRing;  // guest -> host: shots, grenades (M4)
	std::mutex                                           g_eventLock;
	using BulletFireFn = void (*)(void*, float, void*, void*, int);
	using BulletTraceFn = uint64_t (*)(void*, void*, void*, void*, int);
	BulletFireFn                                         g_bulletFireOrig = nullptr;
	BulletTraceFn                                        g_bulletTraceOrig = nullptr;
	std::atomic<uint32_t>                                g_shotId{ 0 }, g_shotsSent{ 0 }, g_bulletsFired{ 0 };
	thread_local bool                                    t_firstTrace = false;
	LARGE_INTEGER                                        g_lastBulletQpc{};
	uint16_t                                             g_pellet = 0;
	std::unordered_map<int64_t, field::Tile>             g_tiles;  // worker thread only
	std::atomic<const field::Snapshot*>                  g_snap{ nullptr };
	std::deque<std::pair<DWORD, const field::Snapshot*>> g_retired;  // freed a few seconds after replacement
	std::atomic_bool                                     g_collision{ false };  // HostState kHostCollision
	std::atomic<uint32_t>                                g_fieldTraces{ 0 }, g_holdTraces{ 0 }, g_epoch{ 0 };
	uint8_t                                              g_hitTemplate[0x30] = {};  // a real MW2 world hit (surface, material)
	std::atomic_bool                                     g_haveTemplate{ false };
	struct Teleport
	{
		bool  pending = false;
		float pos[3] = {};
	};
	Teleport   g_teleport;  // applied to the game playerState inside Pmove

	// Debugging the field: the distinct trace boxes Pmove uses (per stance and mask), and solid starts.
	struct BoundsSeen
	{
		float    b[6];
		int      mask;
		int      stance;
		uint64_t count;
	};
	std::mutex              g_boundsLock;
	std::vector<BoundsSeen> g_boundsSeen;
	std::atomic<uint32_t>   g_solidStarts{ 0 };
	DWORD                   g_lastSolidLog = 0;
	DWORD                   g_lastUnstick = 0;
	std::atomic<uint32_t>   g_unsticks{ 0 };
	std::atomic<int>        g_traceLog{ 0 };  // `tracelog N`: log the next N player-box field traces

	void RecordBounds(const float* a_b, int a_mask)
	{
		int stance = ReadOr<int>(g_exe + kClientActive + kClStance, -1);
		std::lock_guard<std::mutex> lock(g_boundsLock);
		for (auto& e : g_boundsSeen) {
			if (e.mask == a_mask && e.stance == stance && memcmp(e.b, a_b, sizeof(e.b)) == 0) {
				++e.count;
				return;
			}
		}
		if (g_boundsSeen.size() < 64) {
			BoundsSeen e{ {}, a_mask, stance, 1 };
			memcpy(e.b, a_b, sizeof(e.b));
			g_boundsSeen.push_back(e);
		}
	}
	std::mutex g_teleportLock;

	struct Pad
	{
		WORD  buttons = 0;
		float lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
		bool  real = false, virt = false;
	};
	Pad   g_prev;
	DWORD g_bDownAt = 0;
	bool  g_sprintHeld = false;
	bool  g_bProned = false, g_sprintLatched = false, g_rtDown = false, g_ltDown = false, g_aStood = false;
	char  g_lastCmd[64];

	uintptr_t Client(int a_lc) { return g_exe + kClientActive + kClientStride * a_lc; }

	void Cbuf(const char* a_text)
	{
		if (g_cbuf) g_cbuf(0, a_text);
	}

	// ---- controller --------------------------------------------------------------------------------------
	float Stick(SHORT a_v) { return a_v < 0 ? a_v / 32768.0f : a_v / 32767.0f; }

	// Circular dead zone, rescaled so the edge of the dead zone is 0.
	void DeadZone(float& a_x, float& a_y, float a_dz)
	{
		float m = std::sqrt(a_x * a_x + a_y * a_y);
		if (m < a_dz) {
			a_x = a_y = 0;
			return;
		}
		float s = (std::fmin(m, 1.0f) - a_dz) / (1.0f - a_dz) / m;
		a_x *= s;
		a_y *= s;
	}

	Pad ReadPad()
	{
		Pad p;
		proto::PadState v;
		if (g_linked && SeqRead(g_link.Pad(), v) && (v.flags & proto::kPadActive)) {
			p.buttons = v.buttons;
			p.lt = v.leftTrigger / 255.0f;
			p.rt = v.rightTrigger / 255.0f;
			p.lx = Stick(v.thumbLX), p.ly = Stick(v.thumbLY), p.rx = Stick(v.thumbRX), p.ry = Stick(v.thumbRY);
			p.virt = true;
		} else if (g_xinput) {
			XInputState s{};
			DWORD now = GetTickCount();
			if (g_padIndex < 0 && now >= g_padRescan) {  // XInputGetState on empty slots is slow: rescan every 2 s
				g_padRescan = now + 2000;
				for (int i = 0; i < 4 && g_padIndex < 0; ++i)
					if (g_xinput(i, &s) == ERROR_SUCCESS) {
						g_padIndex = i;
						Log("controller found in slot %d", i);
					}
			}
			if (g_padIndex >= 0) {
				if (g_xinput(g_padIndex, &s) == ERROR_SUCCESS) {
					p.buttons = s.buttons;
					p.lt = s.lt / 255.0f;
					p.rt = s.rt / 255.0f;
					p.lx = Stick(s.lx), p.ly = Stick(s.ly), p.rx = Stick(s.rx), p.ry = Stick(s.ry);
					p.real = true;
				} else {
					Log("controller in slot %d disconnected", g_padIndex);
					g_padIndex = -1;
				}
			}
		}
		DeadZone(p.lx, p.ly, 0.24f);
		DeadZone(p.rx, p.ry, 0.20f);
		return p;
	}

	// Controller buttons -> usercmd button bits (CL_CmdButtons' own bits, 0xce200), MW2's stance field and
	// the few real console commands. In this build "+attack" etc. are bind-only (not console commands),
	// so the bits are OR'd into the usercmd after CL_CreateCmd builds it.
	enum CmdBits : uint32_t
	{
		kBitAttack = 0x1, kBitMelee = 0x4,  kBitUseReload = 0x20, kBitUp = 0x400, kBitAds = 0x800,
		kBitBreath = 0x2000, kBitFrag = 0x4000, kBitSmoke = 0x8000,
	};

	void SendEvent(uint32_t a_type, const void* a_p, uint32_t a_n);

	uint32_t Binds(const Pad& a_p, bool a_allowed, int a_lc)
	{
		WORD now = a_allowed ? a_p.buttons : 0;
		WORD was = g_prev.buttons;
		auto pressed = [&](WORD b) { return (now & b) && !(was & b); };
		auto released = [&](WORD b) { return !(now & b) && (was & b); };
		int* stance = reinterpret_cast<int*>(Client(a_lc) + kClStance);
		uint32_t bits = 0;

		bool rt = g_rtDown ? a_p.rt > 0.10f : a_p.rt > 0.25f;  // triggers with hysteresis
		bool lt = g_ltDown ? a_p.lt > 0.10f : a_p.lt > 0.25f;
		if (!a_allowed) rt = lt = false;
		g_rtDown = rt, g_ltDown = lt;
		if (rt) bits |= kBitAttack;          // RT fire
		if (lt) bits |= kBitAds;             // LT aim down sights (hold)
		if (now & 0x4000) bits |= kBitUseReload;  // X reload / use
		if (now & 0x0200) bits |= kBitFrag;       // RB grenade
		if (released(0x0200)) SendEvent(proto::kRecGrenade, nullptr, 0);  // BL2 throws its own grenade then (M4)
		if (now & 0x0100) bits |= kBitSmoke;      // LB tactical
		if (now & 0x0080) bits |= kBitMelee;      // RS click knife
		// A: jump; when crouched or prone it stands you up instead (console style)
		if (pressed(0x1000) && *stance != 0) *stance = 0, g_aStood = true;
		if (released(0x1000)) g_aStood = false;
		if ((now & 0x1000) && !g_aStood) bits |= kBitUp;
		// B: tap = crouch / stand, hold = prone
		DWORD t = GetTickCount();
		if (pressed(0x2000)) g_bDownAt = t, g_bProned = false;
		if ((now & 0x2000) && !g_bProned && t - g_bDownAt > 350) *stance = 2, g_bProned = true;
		if (released(0x2000) && !g_bProned) *stance = *stance == 1 ? 0 : 1;  // stand -> crouch, crouch -> stand, prone -> crouch
		// LS click: sprint, latched until you stop pushing forward; while aiming it holds your breath
		if (pressed(0x0040) && !lt) g_sprintLatched = true, *stance = 0;
		if (g_sprintLatched && (a_p.ly < 0.3f || lt || !a_allowed)) g_sprintLatched = false;
		// sprint: hold MW2's sprint kbutton like a held key (CL_KeyMove sets/clears usercmd bit 0x2 from it)
		if (g_sprintLatched != g_sprintHeld) {
			*reinterpret_cast<uint8_t*>(g_exe + kSprintKbActive) = g_sprintLatched ? 1 : 0;
			g_sprintHeld = g_sprintLatched;
		}
		if (lt && (now & 0x0040)) bits |= kBitBreath;
		if ((pressed(0x8000) || g_weapReq.exchange(false)) && g_weapCycle) g_weapCycle(a_lc, 1);  // Y switch weapon (main thread: we are in CL_CreateCmd)
		// real console commands
		if (pressed(0x0001)) Cbuf("+nightvision\n");            // d-pad up
		if (released(0x0001)) Cbuf("-nightvision\n");
		g_prev = a_p;
		g_prev.buttons = now;
		return bits;
	}

	void MakeSafe();

	// ---- publishing -----------------------------------------------------------------------------------
	bool HostAllowsInput()
	{
		if (!g_linked) return true;
		proto::HostState h;
		if (!Link::Alive(g_link.Hdr()->hostBeatMs) || !SeqRead(g_link.Host(), h)) return true;  // no host: standalone testing
		return !(h.flags & (proto::kHostMenuOpen | proto::kHostLoading));
	}

	void Publish(const uint8_t* a_cmd, const Pad& a_pad)
	{
		if (!g_linked || !g_publish) return;
		auto& hdr = *g_link.Hdr();
		hdr.guestPid = GetCurrentProcessId();
		hdr.guestBeatMs = GetTickCount64();

		proto::GuestState g{};
		g.frame = ++g_frame;
		LARGE_INTEGER q;
		QueryPerformanceCounter(&q);
		g.qpc = q.QuadPart;
		uintptr_t ps = g_psCg.load() ? g_psCg.load() : g_psAny.load();
		bool inWorld = ps && ReadOr<int>(g_exe + kCm + 8, 0) != 0;
		float o[3] = {}, v[3] = {};
		if (inWorld && SafeRead(o, reinterpret_cast<void*>(ps + kPsOrigin), 12) && SafeRead(v, reinterpret_cast<void*>(ps + kPsVelocity), 12)) {
			g.flags |= proto::kGuestInWorld;
			for (int i = 0; i < 3; ++i) g.origin[i] = o[i], g.velocity[i] = v[i];
			g.pmFlags = ReadOr<uint32_t>(ps + kPsPmFlags, 0);
		}
		// View = this frame's client angles (already include the stick turn) + the playerState's delta_angles:
		// what Pmove will produce from this usercmd, without waiting for the next prediction or server frame.
		uintptr_t cl = Client(0);
		float     delta[3] = {};
		SafeRead(g.viewAngles, reinterpret_cast<void*>(cl + kClViewAngles), 12);
		if (g.flags & proto::kGuestInWorld) SafeRead(delta, reinterpret_cast<void*>(ps + kPsDeltaAngles), 12);
		for (int i = 0; i < 3; ++i) g.viewAngles[i] = std::remainder(g.viewAngles[i] + delta[i], 360.0f);
		int stance = ReadOr<int>(cl + kClStance, 0);
		if (stance == 1) g.flags |= proto::kGuestCrouched;
		if (stance == 2) g.flags |= proto::kGuestProne;
		g.viewHeight = stance == 2 ? 11.0f : stance == 1 ? 40.0f : 60.0f;
		if (g_sprintLatched) g.flags |= proto::kGuestSprinting;
		if (g_ltDown) g.flags |= proto::kGuestAds;
		if (a_pad.real) g.flags |= proto::kGuestPadReal;
		if (a_pad.virt) g.flags |= proto::kGuestPadVirtual;
		if (g_hwnd && GetForegroundWindow() == g_hwnd) g.flags |= proto::kGuestFocused;
		if (std::fabs(v[2]) < 1.0f && inWorld) g.flags |= proto::kGuestOnGround;  // TODO: ps groundEntityNum
		g.teleportAck = g_teleportAck;
		g.cmdButtons = ReadOr<uint32_t>(reinterpret_cast<uintptr_t>(a_cmd) + kCmdButtons, 0);
		g.forwardMove = ReadOr<int8_t>(reinterpret_cast<uintptr_t>(a_cmd) + kCmdForward, 0);
		g.rightMove = ReadOr<int8_t>(reinterpret_cast<uintptr_t>(a_cmd) + kCmdRight, 0);
		g.pmoveTraces = g_traces.load();
		std::string map = ReadCStr(ReadOr<uintptr_t>(g_exe + kCm, 0), 63);
		strncpy_s(g.mapName, map.c_str(), _TRUNCATE);
		if (const field::Snapshot* snap = g_snap.load()) {
			g.tilesHeld = static_cast<uint32_t>(snap->Tiles());
			g.boxesHeld = static_cast<uint32_t>(snap->Boxes());
		}
		g.fieldTraces = g_fieldTraces.load();
		g.holdTraces = g_holdTraces.load();
		g.collisionEpoch = g_epoch.load();
		SeqWrite(g_link.Guest(), g);

		// host flags and teleport requests
		// Home = where the player last stood in MW2's own level (no collision, no teleport for 3 s). If the host
		// goes away mid-drive (BL2 quit or crashed), the player is sent back there instead of falling forever
		// through the void below the level (seen 2026-10-03: z -5,000,000 after BL2 was restarted).
		static float       home[3] = {};
		static std::string homeMap;
		static DWORD       lastTeleportReq = 0;
		DWORD              nowMs = GetTickCount();
		if (!g_collision && (g.flags & proto::kGuestInWorld) && nowMs - lastTeleportReq > 3000 && std::fabs(v[2]) < 1.0f && o[2] > kVoidZ)
			memcpy(home, o, sizeof(home)), homeMap = map;

		// A failed SeqRead only means BL2 was writing just then: keep the last state (it used to count as "host
		// gone", flipping collision off and on ~25 times a minute). Gone = no heartbeat for 3 s.
		// Fell out of the level while not driving (nothing holds the player up then): back home, or reload
		// the mission if there is no home on this map. Anchoring a drive down there would leave the player
		// unable to move (float precision), so this never waits for BL2.
		static DWORD voidSince = 0;
		if (!g_collision && (g.flags & proto::kGuestInWorld) && o[2] < kVoidZ) {
			if (!voidSince) voidSince = nowMs;
			if (nowMs - voidSince > 2000) {
				voidSince = nowMs + 15000;  // don't repeat while a reload runs
				if (!homeMap.empty() && homeMap == map && home[2] > kVoidZ) {
					std::lock_guard<std::mutex> lock(g_teleportLock);
					memcpy(g_teleport.pos, home, sizeof(home));
					g_teleport.pending = true;
					lastTeleportReq = nowMs;
					Log("player below the level (z %.0f): back to (%.1f %.1f %.1f)", o[2], home[0], home[1], home[2]);
				} else {
					std::string lvl = map.substr(map.rfind('/') + 1);
					lvl = lvl.substr(0, lvl.find('.'));
					Log("player below the level (z %.0f) with no home: reloading %s", o[2], lvl.c_str());
					if (!lvl.empty()) Cbuf(("spdevmap " + lvl + "\n").c_str());
				}
			}
		} else if (voidSince && voidSince <= nowMs) {
			voidSince = 0;
		}

		proto::HostState h;
		bool             hostAlive = Link::Alive(hdr.hostBeatMs);
		static DWORD     hostSeenMs = nowMs;
		if (hostAlive) hostSeenMs = nowMs;
		if (hostAlive && !SeqRead(g_link.Host(), h)) {
			// torn read: try again next frame
		} else if (hostAlive) {
			// A BL2 session we haven't seen yet (BL2 started or restarted, or MW2 just started): its last
			// teleport request is history, not an order. Applying one stale request once sent the player
			// 97 million units under the level, where MW2 can no longer move it (2026-10-03).
			static uint32_t knownHostPid = 0;
			if (hdr.hostPid != knownHostPid) {
				knownHostPid = hdr.hostPid;
				g_teleportAck = h.teleportSeq;
				Log("BL2 session %u: synced (teleport #%u already done)", hdr.hostPid, h.teleportSeq);
			}
			bool collision = (h.flags & proto::kHostCollision) != 0;
			if (collision && !g_collision) MakeSafe();
			g_collision = collision;
			if (h.teleportSeq != g_teleportAck && h.teleportSeq == 0) {
				g_teleportAck = 0;  // a restarted BL2 (counter back at 0) is not a teleport request: it sent (0 0 0) into the void
			} else if (h.teleportSeq != g_teleportAck) {
				std::lock_guard<std::mutex> lock(g_teleportLock);
				for (int i = 0; i < 3; ++i) g_teleport.pos[i] = static_cast<float>(h.teleportPos[i]);
				g_teleport.pending = true;
				g_teleportAck = h.teleportSeq;
				lastTeleportReq = nowMs;
				Log("teleport #%u requested to (%.1f %.1f %.1f)", h.teleportSeq, h.teleportPos[0], h.teleportPos[1], h.teleportPos[2]);
			}
		} else if (nowMs - hostSeenMs > 3000) {
			if (g_collision && !homeMap.empty() && homeMap == map) {  // host gone mid-drive: back home
				std::lock_guard<std::mutex> lock(g_teleportLock);
				memcpy(g_teleport.pos, home, sizeof(home));
				g_teleport.pending = true;
				lastTeleportReq = nowMs;
				Log("host gone while driving: player back to (%.1f %.1f %.1f)", home[0], home[1], home[2]);
			}
			g_collision = false;  // host gone: back to MW2's own world
		}
	}

	// Cheats on (both sv_cheats dvars), then god mode, no target, no fall damage: while it walks BL2's world,
	// MW2's player must not die (a death reloads MW2's checkpoint and yanks the player out of BL2's world).
	// god/notarget are toggles, so they are sent once per level.
	std::string g_safeMap;

	void MakeSafe()
	{
		for (uintptr_t rva : kSvCheats) {
			uintptr_t d = ReadOr<uintptr_t>(g_exe + rva, 0);
			if (d) *reinterpret_cast<uint8_t*>(d + kDvarValue) = 1;
		}
		Cbuf("set bg_fallDamageMinHeight 99998\nset bg_fallDamageMaxHeight 99999\n");
		// M3: only the hands, gun and MW2's HUD on a magenta clear (captured and keyed for BL2): no sky/world
		// (r_skipPvs), no fog, glow, depth of field or sun
		Cbuf("r_clear 3\nr_clearColor 1 0 1 1\nr_skipPvs 1\nr_fog 0\nr_glow 0\nr_dof_enable 0\nr_drawSun 0\nplayer_sustainAmmo 1\n");  // unlimited MW2 ammo: damage is BL2's business
		std::string map = ReadCStr(ReadOr<uintptr_t>(g_exe + kCm, 0), 63);
		bool fresh = map != g_safeMap;
		if (fresh) {
			Cbuf("god\nnotarget\n");
			g_safeMap = map;
		}
		Log("collision mode on: cheats, no fall damage%s", fresh ? ", god + notarget" : " (god/notarget already on for this level)");
	}

	// ---- automatic MW2 setup ------------------------------------------------------------------------------
	// So a session never needs MW2's menus: at the main menu the guest loads a fixed level (bw2.ini AutoLevel,
	// default contingency: on foot, armed, quiet start) and, once it's in, makes the player safe and gives the
	// loadout (bw2.ini Loadout, weapon names the level's fastfile contains). AutoLevel= (empty) turns it off.
	std::string g_autoLevel, g_loadout;
	DWORD       g_menuSince = 0, g_levelSince = 0;
	bool        g_levelRequested = false;
	std::string g_setupMap;
	int         g_lastCmdTime = 0;  // playerState commandTime: restarts near 0 on every level (re)load

	std::string Narrow(const std::wstring& a_w)  // ini values are ASCII (level and weapon names)
	{
		std::string out;
		for (wchar_t c : a_w) out += static_cast<char>(c < 128 ? c : '?');
		return out;
	}

	void GiveLoadout()
	{
		MakeSafe();
		std::string cmds;
		size_t      start = 0;
		while (start <= g_loadout.size()) {
			size_t end = g_loadout.find(',', start);
			if (end == std::string::npos) end = g_loadout.size();
			std::string w = g_loadout.substr(start, end - start);
			if (!w.empty()) cmds += "give " + w + "\n";
			start = end + 1;
		}
		Cbuf(cmds.c_str());
		Log("setup: loadout given (%s)", g_loadout.c_str());
	}

	void AutoSetup()
	{
		if (g_autoLevel.empty()) return;
		DWORD       now = GetTickCount();
		bool        loaded = ReadOr<int>(g_exe + kCm + 8, 0) != 0 && g_psAny.load() != 0;
		std::string map = ReadCStr(ReadOr<uintptr_t>(g_exe + kCm, 0), 63);
		if (!loaded) {
			g_levelSince = 0;
			if (!g_menuSince) g_menuSince = now;
			if (!g_levelRequested && now - g_menuSince > 8000) {  // let the main menu finish loading first
				g_levelRequested = true;
				Cbuf(("spdevmap " + g_autoLevel + "\n").c_str());  // SP ignores devmap; spdevmap loads with cheats
				Log("setup: loading %s", g_autoLevel.c_str());
			}
			return;
		}
		g_menuSince = 0;
		int cmdTime = ReadOr<int>(g_psAny.load(), 0);
		if (cmdTime + 1000 < g_lastCmdTime) {  // the same level loaded again (restart, checkpoint): a fresh player
			g_setupMap.clear();
			g_safeMap.clear();
			g_levelSince = now;
			Log("setup: level reloaded");
		}
		g_lastCmdTime = cmdTime;
		static uint64_t lastFrame = 0;  // gameplay frames (CL_CreateCmd): the loading briefing produces none
		static DWORD    lastFrameAt = 0;
		if (g_frame != lastFrame) lastFrame = g_frame, lastFrameAt = now;
		if (now - lastFrameAt > 500) {  // no gameplay for half a second: still loading (or paused)
			g_levelSince = 0;
			return;
		}
		if (!g_levelSince) g_levelSince = now;
		if (map != g_setupMap && now - g_levelSince > 6000) {  // 6 s of real gameplay: the level's intro has settled
			g_setupMap = map;
			GiveLoadout();
		}
	}

	// ---- hooks -----------------------------------------------------------------------------------------
	// Pmove's collision query. With collision mode on, BL2's geometry (the streamed field) replaces MW2's
	// world completely: solid queries are answered from it, everything else reports nothing in the way.
	void FieldTrace(uint8_t* a_r, const float* a_s, const float* a_e, const float* a_b, int a_mask)
	{
		g_fieldTraces.fetch_add(1, std::memory_order_relaxed);
		RecordBounds(a_b, a_mask);
		memset(a_r, 0, kTraceBytes);
		const float one = 1.0f;
		if (!(a_mask & 1)) {  // not a solid query (water, triggers, clip probes): nothing there
			memcpy(a_r, &one, 4);
			return;
		}
		float s[3], e[3], h[3];
		for (int i = 0; i < 3; ++i) s[i] = a_s[i] + a_b[i], e[i] = a_e[i] + a_b[i], h[i] = a_b[3 + i];
		field::Result          res;
		const field::Snapshot* snap = g_snap.load(std::memory_order_acquire);
		if (snap) {
			res = snap->Trace(s, e, h);
		} else {
			res.fraction = 0.0f, res.normal[2] = 1.0f, res.hold = true;
		}
		if (res.hold) g_holdTraces.fetch_add(1, std::memory_order_relaxed);
		if (h[0] >= 14.0f && g_traceLog.load(std::memory_order_relaxed) > 0 && g_traceLog.fetch_sub(1) > 0) {
			Log("trace (%.2f %.2f %.2f) -> (%.2f %.2f %.2f) mid z %.1f half (%.1f %.1f) mask 0x%x: frac %.4f n (%.2f %.2f %.2f) start %d all %d hold %d",
				a_s[0], a_s[1], a_s[2], a_e[0], a_e[1], a_e[2], a_b[2], a_b[3], a_b[5], a_mask, res.fraction, res.normal[0], res.normal[1],
				res.normal[2], res.startSolid, res.allSolid, res.hold);
		}
		if (res.allSolid && snap && h[0] >= 14.0f) {
			// The player's own box is embedded (MW2 rounds it like a capsule at ledges; our floor is square
			// cells; scanner artefacts): move it to the nearest free spot - straight up if a step does it, else
			// sideways (never by ignoring solids: that once dropped the player through the floor). Only for traces
			// from where the player actually is, never for Pmove's hypothetical step-up/slide positions.
			uintptr_t ps = g_psAny.load();
			float     o[3];
			DWORD     now = GetTickCount();
			if (ps && SafeRead(o, reinterpret_cast<void*>(ps + kPsOrigin), 12) && std::fabs(o[0] - a_s[0]) < 0.5f &&
				std::fabs(o[1] - a_s[1]) < 0.5f && std::fabs(o[2] - a_s[2]) < 0.5f && now - g_lastUnstick > 150) {
				float off[3];
				if (!snap->FindFree(s, h, 64.0f, off)) {
					if (now - g_lastUnstick > 2000) Log("unstick: no free spot within 64 in of (%.1f %.1f %.1f)", o[0], o[1], o[2]), g_lastUnstick = now;
				} else if (off[0] || off[1] || off[2] > 0.0f) {
					if (off[0] || off[1]) Log("unstick: moved the player (%.1f %.1f %.1f)", off[0], off[1], off[2]);
					std::lock_guard<std::mutex> lock(g_teleportLock);
					g_teleport.pos[0] = o[0] + off[0], g_teleport.pos[1] = o[1] + off[1], g_teleport.pos[2] = o[2] + off[2];
					g_teleport.pending = true;
					g_lastUnstick = now;
					g_unsticks.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
		if (res.startSolid) {
			g_solidStarts.fetch_add(1, std::memory_order_relaxed);
			DWORD now = GetTickCount();
			if (now - g_lastSolidLog > 1000) {
				g_lastSolidLog = now;
				Log("solid start: origin (%.2f %.2f %.2f) -> (%.2f %.2f %.2f) mid (%.2f %.2f %.2f) half (%.2f %.2f %.2f) mask 0x%x all=%d (%u so far)",
					a_s[0], a_s[1], a_s[2], a_e[0], a_e[1], a_e[2], a_b[0], a_b[1], a_b[2], a_b[3], a_b[4], a_b[5], a_mask, res.allSolid,
					g_solidStarts.load());
			}
		}
		bool hit = res.fraction < 1.0f || res.startSolid;
		if (hit && g_haveTemplate) memcpy(a_r, g_hitTemplate, kTraceBytes);
		memcpy(a_r + 0x00, &res.fraction, 4);
		memcpy(a_r + 0x04, res.normal, 12);
		if (hit) {
			const int      contents = 1;
			const uint32_t hitType = 1;
			memcpy(a_r + 0x14, &contents, 4);
			memcpy(a_r + 0x20, &hitType, 4);
			memcpy(a_r + 0x24, &kEntityNumWorld, 2);
			a_r[0x2c] = res.allSolid;
			a_r[0x2d] = res.startSolid;
			a_r[0x2e] = res.normal[2] >= field::kWalkableZ;
		}
	}

	template <int I>
	void TraceHook(void* a_r, const float* a_s, const float* a_e, const float* a_b, int a_pass, int a_mask)
	{
		g_traces.fetch_add(1, std::memory_order_relaxed);
		if (g_collision.load(std::memory_order_relaxed)) {
			FieldTrace(static_cast<uint8_t*>(a_r), a_s, a_e, a_b, a_mask);
			return;
		}
		g_traceOrig[I](a_r, a_s, a_e, a_b, a_pass, a_mask);
		if (!g_haveTemplate) {  // keep a real world hit as the template for our answers
			auto*    r = static_cast<uint8_t*>(a_r);
			float    f;
			uint16_t id;
			memcpy(&f, r, 4);
			memcpy(&id, r + 0x24, 2);
			if (f < 1.0f && id == kEntityNumWorld && r[0x2e]) {
				memcpy(g_hitTemplate, r, kTraceBytes);
				g_haveTemplate = true;
			}
		}
	}

	void SendEvent(uint32_t a_type, const void* a_p, uint32_t a_n)
	{
		if (!g_linked) return;
		std::lock_guard<std::mutex> lock(g_eventLock);  // single producer for the ring
		g_eventRing.Write(a_type, a_p, a_n);
	}

	// One bullet (or pellet), anyone's. The next Bullet_Trace is that bullet's flight.
	void BulletFireHook(void* a, float b, void* c, void* d, int e)
	{
		g_bulletsFired.fetch_add(1, std::memory_order_relaxed);
		t_firstTrace = true;
		g_bulletFireOrig(a, b, c, d, e);
		t_firstTrace = false;
	}

	// The player's bullets go to the host, which fires them through BL2's world. Pellets fired within 2 ms of
	// each other are one shot (a shotgun blast); penetration re-traces are skipped.
	uint64_t BulletTraceHook(void* a, void* b, void* c, void* d, int e)
	{
		if (t_firstTrace && a) {
			t_firstTrace = false;
			int              shooter = -1;
			proto::ShotEvent ev{};
			auto*            bp = static_cast<uint8_t*>(a);
			if (SafeRead(&shooter, bp + kBpShooter, 4) && shooter == 0 && SafeRead(ev.start, bp + kBpStart, 12) &&
				SafeRead(ev.end, bp + kBpEnd, 12)) {
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				bool sameShot = g_lastBulletQpc.QuadPart && (now.QuadPart - g_lastBulletQpc.QuadPart) * 1000 < g_qpcFreq.QuadPart * 2;
				g_lastBulletQpc = now;
				if (!sameShot) g_shotId.fetch_add(1, std::memory_order_relaxed), g_pellet = 0;
				ev.shotId = g_shotId.load(std::memory_order_relaxed);
				ev.pellet = g_pellet++;
				SendEvent(proto::kRecShot, &ev, sizeof(ev));
				g_shotsSent.fetch_add(1, std::memory_order_relaxed);
			}
		}
		return g_bulletTraceOrig(a, b, c, d, e);
	}

	void PmoveHook(void* a_pm)
	{
		uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_exe;
		uintptr_t ps = ReadOr<uintptr_t>(reinterpret_cast<uintptr_t>(a_pm), 0);
		g_psAny = ps;
		if (ret == kPmoveFromCgPredict) g_psCg = ps;
		if (ret == kPmoveFromGame && g_teleport.pending) {
			std::lock_guard<std::mutex> lock(g_teleportLock);
			if (g_teleport.pending) {
				const float zero[3] = { 0, 0, 0 };
				memcpy(reinterpret_cast<void*>(ps + kPsOrigin), g_teleport.pos, 12);
				memcpy(reinterpret_cast<void*>(ps + kPsVelocity), zero, 12);
				g_teleport.pending = false;
				if (GetTickCount() - g_lastUnstick > 50) Log("teleported the player to (%.1f %.1f %.1f)", g_teleport.pos[0], g_teleport.pos[1], g_teleport.pos[2]);
			}
		}
		for (auto& c : g_pmoveCallers) {  // remember the distinct callers for `status`
			if (c == ret) break;
			if (!c) {
				c = ret;
				Log("Pmove called from exe+%llx, ps %p", static_cast<unsigned long long>(ret), reinterpret_cast<void*>(ps));
				break;
			}
		}
		g_pmoveOrig(a_pm);
	}

	uint8_t* CreateCmdHook(uint8_t* a_cmd, int a_lc)
	{
		LARGE_INTEGER q;
		QueryPerformanceCounter(&q);
		float dt = g_lastQpc.QuadPart ? static_cast<float>(q.QuadPart - g_lastQpc.QuadPart) / g_qpcFreq.QuadPart : 0.0f;
		dt = dt < 0 ? 0 : dt > 0.1f ? 0.1f : dt;
		g_lastQpc = q;

		Pad  pad = ReadPad();
		bool allowed = HostAllowsInput();
		uint32_t bits = Binds(pad, allowed, a_lc);
		if (allowed && (pad.rx || pad.ry)) {  // right stick turns the view, with a response curve
			float* ang = reinterpret_cast<float*>(Client(a_lc) + kClViewAngles);
			float  m = std::sqrt(pad.rx * pad.rx + pad.ry * pad.ry);
			float  curve = m;  // components * m: turn rate grows with magnitude^2 (fine aim near centre)
			float  ads = g_ltDown ? 0.45f : 1.0f;
			ang[1] -= pad.rx * curve * 220.0f * ads * dt;  // yaw: stick right turns right (yaw decreases)
			ang[0] -= pad.ry * curve * 170.0f * ads * dt;  // pitch: stick up looks up (pitch decreases)
		}
		uint8_t* cmd = g_createCmdOrig(a_cmd, a_lc);
		*reinterpret_cast<uint32_t*>(cmd + kCmdButtons) |= bits;
		if (allowed && (pad.lx || pad.ly)) {
			cmd[kCmdForward] = static_cast<uint8_t>(static_cast<int8_t>(std::lround(pad.ly * 127.0f)));
			cmd[kCmdRight] = static_cast<uint8_t>(static_cast<int8_t>(std::lround(pad.lx * 127.0f)));
		}
		Publish(cmd, pad);
		return cmd;
	}

	void InstallHooks()
	{
		g_hooksDone = true;
		if (!g_buildOk) return;
		auto* table = reinterpret_cast<uintptr_t*>(g_exe + kPmoveHandlers);
		const uintptr_t want[3] = { kCgTrace, kGTrace, kGTrace };
		const uintptr_t hooks[3] = { reinterpret_cast<uintptr_t>(&TraceHook<0>), reinterpret_cast<uintptr_t>(&TraceHook<1>),
			reinterpret_cast<uintptr_t>(&TraceHook<2>) };
		for (int i = 0; i < 3; ++i) {
			if (table[i * 2] != g_exe + want[i]) {
				Log("pmoveHandlers[%d].trace unexpected, not hooked", i);
				continue;
			}
			g_traceOrig[i] = reinterpret_cast<TraceFn>(table[i * 2]);
			SwapPointer(&table[i * 2], hooks[i]);
		}
		g_pmoveOrig = reinterpret_cast<PmoveFn>(InlineHook(g_exe + kPmove, kPmoveSig, sizeof(kPmoveSig), reinterpret_cast<void*>(&PmoveHook)));
		g_createCmdOrig = reinterpret_cast<CreateCmdFn>(InlineHook(g_exe + kCreateCmd, kCreateCmdSig, sizeof(kCreateCmdSig), reinterpret_cast<void*>(&CreateCmdHook)));
		g_bulletFireOrig = reinterpret_cast<BulletFireFn>(InlineHook(g_exe + kBulletFire, kBulletFireSig, sizeof(kBulletFireSig), reinterpret_cast<void*>(&BulletFireHook)));
		g_bulletTraceOrig = reinterpret_cast<BulletTraceFn>(InlineHook(g_exe + kBulletTrace, kBulletTraceSig, sizeof(kBulletTraceSig), reinterpret_cast<void*>(&BulletTraceHook)));
		Log("M4 hooks: Bullet_Fire %s, bullet trace %s", g_bulletFireOrig ? "ok" : "SIGNATURE MISMATCH", g_bulletTraceOrig ? "ok" : "SIGNATURE MISMATCH");
		Log("hooks on main thread %lu: traces %d/%d/%d, Pmove %s, CL_CreateCmd %s", GetCurrentThreadId(), g_traceOrig[0] != nullptr,
			g_traceOrig[1] != nullptr, g_traceOrig[2] != nullptr, g_pmoveOrig ? "ok" : "SIGNATURE MISMATCH", g_createCmdOrig ? "ok" : "SIGNATURE MISMATCH");
	}

	// ---- commands ------------------------------------------------------------------------------------------
	void Calibrate()
	{
		// Find where playerState keeps the view angles (for later milestones): floats matching cl.viewangles.
		uintptr_t ps = g_psAny.load();
		float     ang[3];
		if (!ps || !SafeRead(ang, reinterpret_cast<void*>(Client(0) + kClViewAngles), 12)) {
			Log("calib: no player state yet");
			return;
		}
		auto near360 = [](float a, float b) { float d = std::fmod(std::fabs(a - b), 360.0f); return d < 0.6f || d > 359.4f; };
		std::string hits;
		for (int off = 0; off < 0x800; off += 4) {
			float f[2];
			if (SafeRead(f, reinterpret_cast<void*>(ps + off), 8) && near360(f[0], ang[0]) && near360(f[1], ang[1]) && std::fabs(ang[1]) > 1.0f) {
				char b[16];
				sprintf_s(b, " +0x%x", off);
				hits += b;
			}
		}
		float o[3];
		SafeRead(o, reinterpret_cast<void*>(ps + kPsOrigin), 12);
		Log("calib: cl.viewangles (%.2f %.2f %.2f), ps %p origin (%.1f %.1f %.1f); view-angle matches in ps:%s", ang[0], ang[1], ang[2],
			reinterpret_cast<void*>(ps), o[0], o[1], o[2], hits.empty() ? " none" : hits.c_str());
	}

	void RunCommand(const std::string& a_line)
	{
		Log("cmd> %s", a_line.c_str());
		if (a_line == "status") {
			proto::GuestState g;
			if (g_linked && SeqRead(g_link.Guest(), g)) {
				Log("status: hooks=%d linked=%d host alive=%d frame=%llu flags=0x%x map=%s origin=(%.1f %.1f %.1f) vel=(%.0f %.0f %.0f) "
					"view=(%.1f %.1f) buttons=0x%x move=%d/%d traces=%u pad=%d collision=%d tiles=%u boxes=%u field=%u hold=%u unsticks=%u player bullets=%u of %u fired frames=%llu capture=%u presents=%u capture errors=%u",
					g_hooksDone.load(), g_linked, Link::Alive(g_link.Hdr()->hostBeatMs), static_cast<unsigned long long>(g.frame), g.flags,
					g.mapName, g.origin[0], g.origin[1], g.origin[2], g.velocity[0], g.velocity[1], g.velocity[2], g.viewAngles[0],
					g.viewAngles[1], g.cmdButtons, g.forwardMove, g.rightMove, g.pmoveTraces, g_padIndex, g_collision.load(), g.tilesHeld,
					g.boxesHeld, g.fieldTraces, g.holdTraces, g_unsticks.load(), g_shotsSent.load(), g_bulletsFired.load(),
					static_cast<unsigned long long>(capture::g_frames.load()), capture::g_map ? capture::Header()->enabled : 0u,
					capture::g_presents.load(), capture::g_errors.load());
			} else {
				Log("status: hooks=%d linked=%d (no guest state yet)", g_hooksDone.load(), g_linked);
			}
		} else if (a_line == "pad") {
			Pad p = ReadPad();
			Log("pad: real=%d virtual=%d buttons=0x%04x lt=%.2f rt=%.2f L=(%.2f %.2f) R=(%.2f %.2f)", p.real, p.virt, p.buttons, p.lt, p.rt, p.lx,
				p.ly, p.rx, p.ry);
		} else if (a_line == "weapnext") {
			g_weapReq = true;
		} else if (a_line.rfind("con ", 0) == 0) {
			Cbuf((a_line.substr(4) + "\n").c_str());
		} else if (a_line.rfind("tp ", 0) == 0) {
			std::lock_guard<std::mutex> lock(g_teleportLock);
			if (sscanf_s(a_line.c_str() + 3, "%f %f %f", &g_teleport.pos[0], &g_teleport.pos[1], &g_teleport.pos[2]) == 3) g_teleport.pending = true;
		} else if (a_line == "publish on" || a_line == "publish off") {
			g_publish = a_line == "publish on";
			Log("publishing %s", g_publish ? "on" : "off");
		} else if (a_line == "bounds") {
			std::lock_guard<std::mutex> lock(g_boundsLock);
			for (const auto& e : g_boundsSeen)
				Log("bounds stance=%d mask=0x%x mid (%.2f %.2f %.2f) half (%.2f %.2f %.2f) -> box z %.2f..%.2f above origin, seen %llu",
					e.stance, e.mask, e.b[0], e.b[1], e.b[2], e.b[3], e.b[4], e.b[5], e.b[2] - e.b[5], e.b[2] + e.b[5],
					static_cast<unsigned long long>(e.count));
			g_boundsSeen.clear();
		} else if (a_line.rfind("tracelog", 0) == 0) {
			g_traceLog = a_line.size() > 9 ? atoi(a_line.c_str() + 9) : 40;
		} else if (a_line == "fieldprobe") {
			uintptr_t              ps = g_psAny.load();
			const field::Snapshot* snap = g_snap.load();
			float                  o[3];
			if (!ps || !snap || !SafeRead(o, reinterpret_cast<void*>(ps + kPsOrigin), 12)) {
				Log("fieldprobe: no player or no field");
			} else {
				const float h[3] = { 15, 15, 35 };
				float       c[3] = { o[0], o[1], o[2] + 35 };
				float       down[3] = { c[0], c[1], c[2] - 40 };
				field::Result r = snap->Trace(c, down, h);
				Log("fieldprobe: origin (%.2f %.2f %.2f) lift %.2f; 40-down trace frac %.4f (floor %.2f) start %d all %d hold %d", o[0], o[1], o[2],
					snap->LiftOut(c, h, 64.0f), r.fraction, c[2] - 40 * r.fraction - 35, r.startSolid, r.allSolid, r.hold);
				for (const auto& b : snap->Near(o[0] - 20, o[1] - 20, o[0] + 20, o[1] + 20))
					if (b.mx[2] > o[2] - 30 && b.mn[2] < o[2] + 80)
						Log("   box x %.1f..%.1f y %.1f..%.1f z %.2f..%.2f", b.mn[0], b.mx[0], b.mn[1], b.mx[1], b.mn[2], b.mx[2]);
			}
		} else if (a_line.rfind("unstick", 0) == 0) {
			float dz = a_line.size() > 8 ? static_cast<float>(atof(a_line.c_str() + 8)) : 12.0f;
			uintptr_t ps = g_psAny.load();
			std::lock_guard<std::mutex> lock(g_teleportLock);
			if (ps && SafeRead(g_teleport.pos, reinterpret_cast<void*>(ps + kPsOrigin), 12)) {
				g_teleport.pos[2] += dz;
				g_teleport.pending = true;
				Log("unstick: lifting the player %.1f", dz);
			}
		} else if (a_line == "setup") {
			GiveLoadout();
		} else if (a_line.rfind("level ", 0) == 0) {
			Cbuf(("spdevmap " + a_line.substr(6) + "\n").c_str());
		} else if (a_line == "calib") {
			Calibrate();
		} else {
			Log("unknown command (status | pad | con <cmd> | tp x y z [yaw] | calib | publish on|off | bounds | unstick [dz] | tracelog [n] | fieldprobe | setup | level <map>)");
		}
	}

	// Tiles from the host -> a new immutable snapshot for the trace hook (old ones freed a few seconds later).
	void DrainCollision()
	{
		bool changed = false;
		g_colRing.Drain([&](uint32_t a_type, const uint8_t* a_p, uint32_t a_n) {
			if (a_type == proto::kRecClearAll) {
				g_tiles.clear();
				changed = true;
			} else if (a_type == proto::kRecTile && a_n >= sizeof(proto::TileHeader)) {
				proto::TileHeader th;
				memcpy(&th, a_p, sizeof(th));
				if (a_n < sizeof(th) + th.count * sizeof(proto::TileBox)) return;
				int64_t key = (static_cast<int64_t>(th.tx) << 32) ^ static_cast<uint32_t>(th.ty);
				g_epoch = th.epoch;
				if (th.count == 0 && th.bounds[3] <= th.bounds[0]) {
					g_tiles.erase(key);  // empty bounds: drop the tile
				} else {
					field::Tile& t = g_tiles[key];
					t.tx = th.tx, t.ty = th.ty;
					memcpy(t.bounds, th.bounds, sizeof(t.bounds));
					t.boxes.resize(th.count);
					if (th.count) memcpy(t.boxes.data(), a_p + sizeof(th), th.count * sizeof(proto::TileBox));
				}
				changed = true;
			}
		});
		DWORD now = GetTickCount();
		if (changed) {
			auto*                  snap = new field::Snapshot(g_tiles);
			const field::Snapshot* old = g_snap.exchange(snap, std::memory_order_acq_rel);
			if (old) g_retired.emplace_back(now, old);
		}
		while (!g_retired.empty() && now - g_retired.front().first > 3000) {
			delete g_retired.front().second;
			g_retired.pop_front();
		}
	}

	BOOL CALLBACK FindMain(HWND a_hwnd, LPARAM)
	{
		DWORD pid;
		DWORD tid = GetWindowThreadProcessId(a_hwnd, &pid);
		char  title[64];
		if (pid == GetCurrentProcessId() && IsWindowVisible(a_hwnd) && GetWindowTextA(a_hwnd, title, sizeof(title)) &&
			strcmp(title, "Modern Warfare 2") == 0) {
			g_hwnd = a_hwnd;
			g_mainTid = tid;
			return FALSE;
		}
		return TRUE;
	}

	DWORD WINAPI Worker(LPVOID)
	{
		uint32_t stamp = ExeTimestamp();
		g_buildOk = stamp == kExeStamp;
		Log("Border Warfare 2 MW2 guest. exe %p, TimeDateStamp 0x%08x (%s)", reinterpret_cast<void*>(g_exe), stamp,
			g_buildOk ? "known build" : "UNKNOWN build: doing nothing");
		if (!g_buildOk) return 0;
		if (memcmp(reinterpret_cast<void*>(g_exe + kCbufAddText), kCbufSig, sizeof(kCbufSig)) == 0) g_cbuf = reinterpret_cast<CbufFn>(g_exe + kCbufAddText);
		if (memcmp(reinterpret_cast<void*>(g_exe + kWeapCycle), kWeapCycleSig, sizeof(kWeapCycleSig)) == 0) g_weapCycle = reinterpret_cast<WeapCycleFn>(g_exe + kWeapCycle);
		QueryPerformanceFrequency(&g_qpcFreq);
		if (HMODULE xi = LoadLibraryW(L"xinput1_4.dll")) g_xinput = reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState"));
		g_linked = g_link.Open();
		if (g_linked) {
			g_cmdRing = Ring(g_link.Base() + proto::kOffCmdRing, proto::kCmdRingBytes), g_cmdRing.Discard();
			g_colRing = Ring(g_link.Base() + proto::kOffCollisionRing, proto::kCollisionRingBytes), g_colRing.Discard();
			g_eventRing = Ring(g_link.Base() + proto::kOffEventRing, proto::kEventRingBytes);
		}
		g_autoLevel = Narrow(IniString(L"AutoLevel", L"contingency"));
		g_loadout = Narrow(IniString(L"Loadout", L"m4_grunt,usp,fraggrenade,flash_grenade"));
		Log("link %s, XInput %s, Cbuf_AddText %s, weapon cycle %s", g_linked ? "open" : "FAILED", g_xinput ? "loaded" : "missing", g_cbuf ? "ok" : "mismatch",
			g_weapCycle ? "ok" : "mismatch");

		while (!g_hwnd) {
			EnumWindows(FindMain, 0);
			if (!g_hwnd) Sleep(250);
		}
		Log("main window %p, thread %lu; hooks go in from that thread's next timeGetTime call", g_hwnd, g_mainTid.load());
		const bool capOn = IniInt(L"Capture", 1) != 0;  // bw2.ini Capture=0: no frame sharing (MW2 untouched by D3D hooks)
		if (!capOn) Log("capture: off (bw2.ini Capture=0)");
		else if (!capture::Install()) Log("capture: not installed (M3 frame sharing off)");
		for (DWORD next = GetTickCount() + 5000;;) {
			Sleep(20);
			for (auto& c : TakeCommands(L"mw")) RunCommand(c);
			if (g_linked) DrainCollision();
			if (g_hooksDone) AutoSetup();
			if (g_linked) {
				g_cmdRing.Drain([](uint32_t a_type, const uint8_t* a_p, uint32_t a_n) {
					if (a_type == proto::kRecCommand) RunCommand(std::string(reinterpret_cast<const char*>(a_p), a_n));
				});
			}
			if (GetTickCount() >= next) {  // occasional heartbeat line in the log
				next = GetTickCount() + 30000;
				RunCommand("status");
			}
		}
	}

	using TimeGetTimeFn = DWORD(WINAPI*)();
	TimeGetTimeFn g_realTimeGetTime = nullptr;
}

extern "C" DWORD WINAPI bw2_timeGetTime()
{
	if (!g_realTimeGetTime) {
		wchar_t path[MAX_PATH];
		GetSystemDirectoryW(path, MAX_PATH);
		wcscat_s(path, L"\\winmm.dll");
		g_realTimeGetTime = reinterpret_cast<TimeGetTimeFn>(GetProcAddress(LoadLibraryW(path), "timeGetTime"));
	}
	if (!g_hooksDone.load(std::memory_order_relaxed) && g_mainTid.load(std::memory_order_relaxed) == GetCurrentThreadId()) InstallHooks();
	return g_realTimeGetTime();
}

BOOL APIENTRY DllMain(HMODULE a_module, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(a_module);
		InitLog(a_module, L"mw_guest");
		if (HANDLE t = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr)) CloseHandle(t);
	}
	return TRUE;
}
