// Prints every protocol field offset ("Struct.field 0xNN") for tools/bw2_proto.py to compare, and
// self-tests the seqlock and ring on a private mapping. Built for x86 and x64 (build.bat).
// Exit code != 0 on a failed self-test. Never touches the live mapping.
#include <cstddef>
#include <cstdio>
#include <vector>

#include "../src/common/link.h"

using namespace bw2;
using namespace bw2::proto;

#define F(S, f) printf(#S "." #f " 0x%x\n", static_cast<unsigned>(offsetof(S, f)))
#define Z(S) printf(#S ".sizeof 0x%x\n", static_cast<unsigned>(sizeof(S)))

static int Fail(const char* a_what)
{
	fprintf(stderr, "SELFTEST FAILED: %s\n", a_what);
	return 1;
}

int main()
{
	F(Header, magic); F(Header, version); F(Header, hostPid); F(Header, guestPid); F(Header, hostBeatMs); F(Header, guestBeatMs); Z(Header);
	F(HostState, seq); F(HostState, flags); F(HostState, frame); F(HostState, deltaTime); F(HostState, teleportSeq);
	F(HostState, teleportPos); F(HostState, teleportYaw); F(HostState, worldId); F(HostState, puppetPos); F(HostState, puppetYaw);
	F(HostState, collisionEpoch); Z(HostState);
	F(PadState, seq); F(PadState, flags); F(PadState, packet); F(PadState, buttons); F(PadState, leftTrigger); F(PadState, rightTrigger);
	F(PadState, thumbLX); F(PadState, thumbLY); F(PadState, thumbRX); F(PadState, thumbRY); Z(PadState);
	F(GuestState, seq); F(GuestState, flags); F(GuestState, frame); F(GuestState, qpc); F(GuestState, origin); F(GuestState, velocity);
	F(GuestState, viewAngles); F(GuestState, viewHeight); F(GuestState, pmFlags); F(GuestState, teleportAck); F(GuestState, cmdButtons);
	F(GuestState, forwardMove); F(GuestState, rightMove); F(GuestState, reserved0); F(GuestState, pmoveTraces); F(GuestState, mapName);
	F(GuestState, tilesHeld); F(GuestState, boxesHeld); F(GuestState, fieldTraces); F(GuestState, holdTraces); F(GuestState, collisionEpoch);
	Z(GuestState);
	F(TileHeader, tx); F(TileHeader, ty); F(TileHeader, epoch); F(TileHeader, count); F(TileHeader, bounds); Z(TileHeader);
	F(ShotEvent, shotId); F(ShotEvent, pellet); F(ShotEvent, reserved); F(ShotEvent, start); F(ShotEvent, end); Z(ShotEvent);

	// ---- self tests on a private (unnamed) buffer ----
	std::vector<uint8_t> buf(0x10000 + 0x80, 0);
	Ring w(buf.data(), static_cast<uint32_t>(buf.size())), r(buf.data(), static_cast<uint32_t>(buf.size()));
	uint32_t written = 0, read = 0, expect = 0;
	char     msg[300];
	for (int round = 0; round < 2000; ++round) {
		int len = snprintf(msg, sizeof(msg), "command %u %*s", written, (round * 37) % 250, "x");
		if (w.Write(kRecCommand, msg, static_cast<uint32_t>(len))) ++written;
		if (round % 3 == 0) {
			r.Drain([&](uint32_t a_type, const uint8_t* a_p, uint32_t a_n) {
				unsigned id = 0;
				if (a_type != kRecCommand || sscanf_s(reinterpret_cast<const char*>(a_p), "command %u", &id) != 1 || id != expect || a_n < 9) expect = ~0u;
				++expect;
				++read;
			});
			if (expect == 0) return Fail("ring order");
		}
	}
	r.Drain([&](uint32_t, const uint8_t*, uint32_t) { ++read; });
	if (read != written || written < 1000) return Fail("ring count");

	GuestState g{}, out{};
	g.frame = 42;
	g.origin[2] = 123.5;
	GuestState src = g;
	SeqWrite(&g, src);
	if (!SeqRead(&g, out) || out.frame != 42 || out.origin[2] != 123.5 || (out.seq & 1)) return Fail("seqlock");
	fprintf(stderr, "selftest OK (%u ring records)\n", written);
	return 0;
}
