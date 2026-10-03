// Border Warfare 2 shared-memory protocol: Borderlands 2 (host, x86) <-> MW2 SP (guest, x64).
//
// Design after SkyCraft's (MIT): one named mapping holding a header with heartbeats, latest-value
// slots under a seqlock (per-frame state) and SPSC byte rings (events, streams). The layout is
// fixed-width with explicit offsets so the 32-bit host, the 64-bit guest and Python (tools/bw2_proto.py,
// sdk_mods/bw2/bw2_proto.py) agree byte for byte; tests/layout_test checks it on both architectures.
//
// Little-endian. Whoever starts first creates the mapping. Positions are GUEST space (MW2 world
// units = inches, Z up, right-handed, yaw counter-clockwise from +X) unless a field says otherwise;
// the host converts (see tools/bw2_coords.py and sdk_mods/bw2/coords.py).
#pragma once

#include <cstddef>
#include <cstdint>

namespace bw2::proto
{
	inline constexpr std::uint32_t kMagic = 0x4C325742;  // "BW2L"
	inline constexpr std::uint32_t kVersion = 2;
	inline constexpr wchar_t       kMappingName[] = L"Local\\BW2_v1";

	inline constexpr std::uint32_t kOffHeader = 0x0;
	inline constexpr std::uint32_t kOffHost = 0x100;        // HostState    host  -> guest
	inline constexpr std::uint32_t kOffPad = 0x200;         // PadState     tools/host -> guest
	inline constexpr std::uint32_t kOffGuest = 0x300;       // GuestState   guest -> host
	inline constexpr std::uint32_t kOffEventRing = 0x1000;  // guest -> host events (shots, ...)
	inline constexpr std::uint32_t kEventRingBytes = 0x10000;
	inline constexpr std::uint32_t kOffCmdRing = 0x11000;   // host/tools -> guest commands
	inline constexpr std::uint32_t kCmdRingBytes = 0xF000;
	inline constexpr std::uint32_t kOffCollisionRing = 0x20000;  // host -> guest collision (M2)
	inline constexpr std::uint32_t kCollisionRingBytes = 16u << 20;
	inline constexpr std::uint32_t kMappingBytes = kOffCollisionRing + kCollisionRingBytes;

	inline constexpr std::uint64_t kHeartbeatTimeoutMs = 1500;

	// ---- header @0x0 ---------------------------------------------------------------------------
	struct Header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t hostPid;
		std::uint32_t guestPid;
		std::uint64_t hostBeatMs;   // GetTickCount64() at the host's last tick
		std::uint64_t guestBeatMs;  // GetTickCount64() at the guest's last publish
	};
	static_assert(sizeof(Header) == 0x20);

	// ---- host -> guest @0x100 (seqlock) --------------------------------------------------------
	enum HostFlags : std::uint32_t
	{
		kHostInGame = 1u << 0,       // a BL2 level is loaded and the pawn exists
		kHostMenuOpen = 1u << 1,     // a BL2 menu owns input: the guest drops controller input
		kHostLoading = 1u << 2,      // loading screen
		kHostDrivePuppet = 1u << 3,  // BL2's pawn follows GuestState
		kHostCollision = 1u << 4,    // the guest answers Pmove's traces from the streamed BL2 collision (M2)
	};

	struct HostState
	{
		std::uint32_t seq;
		std::uint32_t flags;           // HostFlags
		std::uint64_t frame;           // host ticks
		float         deltaTime;       // seconds
		std::uint32_t teleportSeq;     // the guest moves its player to teleportPos when this changes
		double        teleportPos[3];  // guest space, feet
		float         teleportYaw;     // degrees, guest convention
		std::uint32_t worldId;         // changes when BL2 changes level (drops collision)
		double        puppetPos[3];    // where BL2's pawn feet are now, guest space
		float         puppetYaw;       // degrees, guest convention
		std::uint32_t collisionEpoch;  // bumps when the host clears all collision (M2)
		std::uint8_t  reserved[0x100 - 0x58];
	};
	static_assert(offsetof(HostState, teleportPos) == 0x18);
	static_assert(offsetof(HostState, puppetPos) == 0x38);
	static_assert(sizeof(HostState) == 0x100);

	// ---- virtual pad @0x200 (tools/host -> guest; seqlock) ---------------------------------------
	// XInput layout. When kPadActive is set the guest uses this instead of the real controller
	// (fake_bl2.py scripts input through it).
	enum PadFlags : std::uint32_t
	{
		kPadActive = 1u << 0,
	};

	struct PadState
	{
		std::uint32_t seq;
		std::uint32_t flags;    // PadFlags
		std::uint32_t packet;   // increments on change
		std::uint16_t buttons;  // XINPUT_GAMEPAD_* bits
		std::uint8_t  leftTrigger;
		std::uint8_t  rightTrigger;
		std::int16_t  thumbLX, thumbLY, thumbRX, thumbRY;
		std::uint8_t  reserved[0x100 - 0x18];
	};
	static_assert(offsetof(PadState, thumbLX) == 0x10);
	static_assert(sizeof(PadState) == 0x100);

	// ---- guest -> host @0x300 (seqlock) ----------------------------------------------------------
	enum GuestFlags : std::uint32_t
	{
		kGuestInWorld = 1u << 0,   // a level is loaded and the player state is valid
		kGuestOnGround = 1u << 1,
		kGuestCrouched = 1u << 2,
		kGuestProne = 1u << 3,
		kGuestSprinting = 1u << 4,
		kGuestAds = 1u << 5,
		kGuestPadReal = 1u << 6,     // input this frame came from a real XInput controller
		kGuestPadVirtual = 1u << 7,  // input this frame came from PadState
		kGuestFocused = 1u << 8,     // MW2's window has OS focus
	};

	struct GuestState
	{
		std::uint32_t seq;
		std::uint32_t flags;          // GuestFlags
		std::uint64_t frame;          // guest publishes
		std::int64_t  qpc;            // QueryPerformanceCounter at publish
		double        origin[3];      // player feet, guest space
		float         velocity[3];    // units/s
		float         viewAngles[3];  // degrees: pitch (down +), yaw, roll  (MW2 convention)
		float         viewHeight;     // eye above feet, units
		std::uint32_t pmFlags;        // playerState pm_flags (raw, for debugging)
		std::uint32_t teleportAck;    // last HostState::teleportSeq applied
		std::uint32_t cmdButtons;     // usercmd buttons sent this frame
		std::int8_t   forwardMove;    // usercmd forwardmove / rightmove sent this frame
		std::int8_t   rightMove;
		std::uint16_t reserved0;
		std::uint32_t pmoveTraces;    // pmove collision traces so far (both sides)
		char          mapName[64];    // e.g. "maps/roadkill.d3dbsp"
		std::uint32_t tilesHeld;      // BL2 collision tiles the guest holds
		std::uint32_t boxesHeld;      // collision boxes in them
		std::uint32_t fieldTraces;    // Pmove traces answered from BL2 collision so far
		std::uint32_t holdTraces;     // ... of those, answered "blocked" because no tile covered them yet
		std::uint32_t collisionEpoch; // last HostState::collisionEpoch the guest applied
		std::uint8_t  reserved[0x200 - 0xB4];
	};
	static_assert(offsetof(GuestState, origin) == 0x18);
	static_assert(offsetof(GuestState, velocity) == 0x30);
	static_assert(offsetof(GuestState, viewAngles) == 0x3C);
	static_assert(offsetof(GuestState, mapName) == 0x60);
	static_assert(offsetof(GuestState, tilesHeld) == 0xA0);
	static_assert(sizeof(GuestState) == 0x200);

	// ---- SPSC byte rings ---------------------------------------------------------------------------
	// [u64 head = total bytes written] pad to 0x40 [u64 tail = total bytes consumed] pad to 0x80, data.
	// A record is RingRecord + payload, padded to 8 bytes, and never wraps: a writer that would cross
	// the end writes a kRecPad record over the rest (or skips < 8 bytes) and continues at the start.
	inline constexpr std::uint32_t kRingHeadOff = 0x00;
	inline constexpr std::uint32_t kRingTailOff = 0x40;
	inline constexpr std::uint32_t kRingDataOff = 0x80;

	enum RecordType : std::uint32_t
	{
		kRecPad = 0,
		kRecCommand = 1,  // ASCII command line (no terminator), same syntax as logs\mw_cmd.txt
		kRecTile = 2,     // TileHeader + count * TileBox: replaces that tile's collision (count 0 = drop it)
		kRecClearAll = 3, // no payload: drop all collision (level change, drive off)
		kRecShot = 4,     // guest -> host event ring: ShotEvent, one per bullet MW2's player fires (M4)
		kRecGrenade = 5,  // guest -> host event ring: no payload, MW2's player threw a frag (M4)
	};

	struct RingRecord
	{
		std::uint32_t type;
		std::uint32_t bytes;  // payload bytes
	};
	static_assert(sizeof(RingRecord) == 8);

	// ---- MW2's first-person frame (guest -> host, M3) --------------------------------------------
	// A separate mapping. MW2 renders its hands and gun over a magenta clear in the void below its own level;
	// the guest copies each presented frame, turns the key colour into alpha (BGRA, straight alpha) and
	// writes it to the slot that isn't being read; the host uploads the newest one and draws it over BL2.
	inline constexpr wchar_t       kFrameMappingName[] = L"Local\\BW2_frame_v1";
	inline constexpr std::uint32_t kFrameMaxWidth = 2560, kFrameMaxHeight = 1440;
	inline constexpr std::uint32_t kFrameSlotBytes = kFrameMaxWidth * kFrameMaxHeight * 4;
	inline constexpr std::uint32_t kFrameHeaderBytes = 0x100;
	inline constexpr std::uint32_t kFrameMappingBytes = kFrameHeaderBytes + 2 * kFrameSlotBytes;

	struct FrameHeader
	{
		std::uint32_t seq;      // seqlock over the fields below (not the pixels)
		std::uint32_t slot;     // 0/1: the slot holding the newest complete frame
		std::uint64_t frameId;  // frames written so far (0 = none yet)
		std::int64_t  qpc;      // when MW2 presented it
		std::uint32_t width;    // pixels (rows are width * 4 bytes, no padding)
		std::uint32_t height;
		std::uint32_t reading;  // host: slot it is copying from right now (0/1), or 0xffffffff
		std::uint32_t enabled;  // host: 1 = the guest should capture
		std::uint8_t  reserved[kFrameHeaderBytes - 0x28];
	};
	static_assert(sizeof(FrameHeader) == kFrameHeaderBytes);

	// ---- collision tiles (host -> guest, collision ring) -----------------------------------------
	// BL2 is scanned in square tiles; each tile becomes solid axis-aligned boxes in guest space.
	struct TileHeader
	{
		std::int32_t  tx, ty;     // tile key (host grid)
		std::uint32_t epoch;      // HostState::collisionEpoch it belongs to
		std::uint32_t count;      // TileBox records that follow
		float         bounds[6];  // tile AABB, guest space: min xyz, max xyz (space the tile is authoritative for)
	};
	static_assert(sizeof(TileHeader) == 0x28);

	// ---- gunplay events (guest -> host, event ring) -----------------------------------------------
	// One per bullet (shotguns: one per pellet), with MW2's own spread applied. Guest space.
	struct ShotEvent
	{
		std::uint32_t shotId;  // same for every pellet of one trigger pull
		std::uint16_t pellet;  // 0.. within the shot
		std::uint16_t reserved;
		float         start[3];
		float         end[3];
	};
	static_assert(sizeof(ShotEvent) == 0x20);

	struct TileBox
	{
		float mn[3];
		float mx[3];
	};
	static_assert(sizeof(TileBox) == 0x18);
}
