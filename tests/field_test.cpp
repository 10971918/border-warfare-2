// Unit tests for src/common/field.h (the guest's BL2 collision). Exit code != 0 on failure.
#include <cmath>
#include <cstdio>

#include "../src/common/field.h"

using namespace bw2::field;

static int g_fail = 0;
#define EXPECT(cond, ...)                                    \
	do {                                                     \
		if (!(cond)) {                                       \
			printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
			printf(__VA_ARGS__);                             \
			printf("\n");                                    \
			++g_fail;                                        \
		}                                                    \
	} while (0)

int main()
{
	std::unordered_map<int64_t, Tile> tiles;
	Tile t;
	t.bounds[0] = 0, t.bounds[1] = 0, t.bounds[2] = -500, t.bounds[3] = 1000, t.bounds[4] = 1000, t.bounds[5] = 500;
	t.boxes.push_back({ { 0, 0, -16 }, { 1000, 1000, 0 } });      // floor, top at z 0
	t.boxes.push_back({ { 500, 0, 0 }, { 512, 1000, 200 } });      // wall at x 500..512
	t.boxes.push_back({ { 200, 0, 0 }, { 300, 1000, 16 } });       // 16-unit step at x 200..300
	t.boxes.push_back({ { 600, 600, 100 }, { 700, 700, 110 } });   // floating slab (ceiling / platform)
	tiles[1] = t;
	Snapshot snap(tiles);
	const float half[3] = { 15, 15, 35 };  // MW2 standing player box (mid 0,0,35)

	// 1. ground probe from rest: player box bottom 0.25 above the floor, probe 0.25 down -> hits, normal up
	{
		float s[3] = { 100, 100, 35.25f }, e[3] = { 100, 100, 35.0f };
		Result r = snap.Trace(s, e, half);
		EXPECT(r.fraction < 1.0f && r.normal[2] == 1.0f && !r.startSolid, "frac %.3f n %.1f", r.fraction, r.normal[2]);
	}
	// 2. falling from 100 up lands on the floor, stopping just above it
	{
		float s[3] = { 100, 100, 135 }, e[3] = { 100, 100, -65 };
		Result r = snap.Trace(s, e, half);
		float z = s[2] + (e[2] - s[2]) * r.fraction - 35.0f;  // feet
		EXPECT(r.fraction > 0.49f && r.fraction < 0.5f && z >= 0.0f && z < 0.2f && r.normal[2] == 1.0f, "frac %.4f feet %.3f", r.fraction, z);
	}
	// 3. walking +x into the wall stops before x = 500 - 15
	{
		float s[3] = { 400, 100, 36 }, e[3] = { 600, 100, 36 };
		Result r = snap.Trace(s, e, half);
		float x = s[0] + (e[0] - s[0]) * r.fraction;
		EXPECT(r.normal[0] == -1.0f && x < 485.0f && x > 484.7f, "frac %.4f x %.3f n %.0f", r.fraction, x, r.normal[0]);
	}
	// 4. walking into the step face (16 high) at knee height: blocked by its -x face
	{
		float s[3] = { 100, 100, 36 }, e[3] = { 250, 100, 36 };
		Result r = snap.Trace(s, e, half);
		EXPECT(r.normal[0] == -1.0f && r.fraction < 1.0f, "frac %.4f n %.0f %.0f %.0f", r.fraction, r.normal[0], r.normal[1], r.normal[2]);
	}
	// 5. same move with the box raised above the step (MW2's step-up trace) is clear
	{
		float s[3] = { 100, 100, 36 + 18 }, e[3] = { 250, 100, 36 + 18 };
		Result r = snap.Trace(s, e, half);
		EXPECT(r.fraction == 1.0f, "frac %.4f", r.fraction);
	}
	// 6. starting inside the wall: startsolid / allsolid
	{
		float s[3] = { 506, 100, 100 }, e[3] = { 506, 100, 101 };
		Result r = snap.Trace(s, e, half);
		EXPECT(r.startSolid && r.allSolid && r.fraction == 0.0f, "start %d all %d", r.startSolid, r.allSolid);
	}
	// 7. jumping up under the slab hits its underside, normal down
	{
		float s[3] = { 650, 650, 36 }, e[3] = { 650, 650, 90 };
		Result r = snap.Trace(s, e, half);
		EXPECT(r.normal[2] == -1.0f && r.fraction < 1.0f, "frac %.4f n %.0f", r.fraction, r.normal[2]);
	}
	// 8. outside every tile: hold (blocked, not solid)
	{
		float s[3] = { 1100, 100, 36 }, e[3] = { 1200, 100, 36 };
		Result r = snap.Trace(s, e, half);
		EXPECT(r.hold && r.fraction == 0.0f && !r.startSolid, "hold %d frac %.3f", r.hold, r.fraction);
	}
	// 9. a move that leaves the tile is held too (no walking off the edge of known space)
	{
		float s[3] = { 980, 100, 36 }, e[3] = { 1010, 100, 36 };
		EXPECT(snap.Trace(s, e, half).hold, "edge");
	}
	// 10. a point trace (zero extents) down onto the floor
	{
		const float zero[3] = { 0, 0, 0 };
		float s[3] = { 50, 50, 10 }, e[3] = { 50, 50, -10 };
		Result r = snap.Trace(s, e, zero);
		EXPECT(std::fabs(r.fraction - (10.0f - kSurfEps) / 20.0f) < 1e-4f, "frac %.4f", r.fraction);
	}
	// 11. a player settled 0.8 into the floor lifts out by ~1, onto the floor top
	{
		float c[3] = { 100, 100, 35 - 0.8f };
		float lift = snap.LiftOut(c, half, 24.0f);
		EXPECT(lift > 0.8f && lift < 1.2f, "lift %.3f", lift);
	}
	// 12. ... standing at the foot of the 16-unit step, overlapping it by 2: lifted onto the step (16 + a bit)
	{
		float c[3] = { 200 - 13, 100, 35 };
		float lift = snap.LiftOut(c, half, 24.0f);
		EXPECT(lift > 16.0f && lift < 16.5f, "lift %.3f", lift);
	}
	// 13. inside the tall wall: too far to lift -> -1; free space -> 0
	{
		float c[3] = { 506, 100, 100 }, f[3] = { 100, 100, 36 };
		EXPECT(snap.LiftOut(c, half, 24.0f) < 0 && snap.LiftOut(f, half, 24.0f) == 0.0f, "wall/free");
	}
	// 14. escape: from inside the tall wall, moving out of it is free; normal tracing says start solid
	{
		float  s[3] = { 506, 100, 100 }, e[3] = { 530, 100, 100 };
		Result stuck = snap.Trace(s, e, half), out = snap.Trace(s, e, half, true);
		EXPECT(stuck.startSolid && !out.startSolid && out.fraction == 1.0f, "stuck %d out frac %.3f", stuck.startSolid, out.fraction);
	}
	// 15. FindFree: inside the wall (too deep to lift) -> nearest free spot sideways, out of the wall
	{
		float c[3] = { 506, 100, 36 }, off[3];
		bool  ok = snap.FindFree(c, half, 64.0f, off);
		float nc[3] = { c[0] + off[0], c[1] + off[1], c[2] + off[2] };
		EXPECT(ok && snap.LiftOut(nc, half, 0.0f) == 0.0f && (nc[0] < 485 || nc[0] > 527), "ok %d at %.1f %.1f %.1f", ok, nc[0], nc[1], nc[2]);
	}
	printf("field_test: %s (%zu boxes)\n", g_fail ? "FAILED" : "OK", snap.Boxes());
	return g_fail ? 1 : 0;
}
