// End-to-end offline check: boxes from tests/test_scan.py (synthetic BL2 world, scanned and converted to
// MW2 space) loaded into the guest's collision field, walked with a simplified IW-style step/slide mover.
//   python tests/test_scan.py --export bin/scan_boxes.txt && bin/walk_test.exe bin/scan_boxes.txt
#include <cmath>
#include <cstdio>

#include "../src/common/field.h"

using namespace bw2::field;

static const float kHalf[3] = { 15, 15, 35 };  // MW2 standing player
static const float kStep = 18.0f;               // jump_stepSize

struct Mover
{
	const Snapshot& w;
	float           p[3];  // box centre (feet + 35)

	void Trace(const float a[3], const float b[3], Result& r, float out[3]) const
	{
		r = w.Trace(a, b, kHalf);
		for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * r.fraction;
	}

	void Gravity()
	{
		float  e[3] = { p[0], p[1], p[2] - 40 }, o[3];
		Result r;
		Trace(p, e, r, o);
		if (!r.hold) p[2] = o[2];
	}

	void Move(float a_dx, float a_dy)
	{
		float  e[3] = { p[0] + a_dx, p[1] + a_dy, p[2] }, o[3];
		Result r;
		Trace(p, e, r, o);
		if (r.fraction == 1.0f) {
			p[0] = o[0], p[1] = o[1];
			return;
		}
		// step up, move, step down (PM_StepSlideMove)
		float  up[3] = { p[0], p[1], p[2] + kStep }, u[3];
		Result ru;
		Trace(p, up, ru, u);
		float  e2[3] = { u[0] + a_dx, u[1] + a_dy, u[2] }, o2[3];
		Result r2;
		Trace(u, e2, r2, o2);
		if (r2.fraction > r.fraction) {
			float  d[3] = { o2[0], o2[1], o2[2] - kStep - 1 }, o3[3];
			Result r3;
			Trace(o2, d, r3, o3);
			if (r3.normal[2] >= kWalkableZ || r3.fraction == 1.0f) {
				for (int i = 0; i < 3; ++i) p[i] = o3[i];
				return;
			}
		}
		p[0] = o[0], p[1] = o[1];  // blocked: stop at the obstacle
	}

	float Feet() const { return p[2] - 35; }
};

static int Walk(const Snapshot& w, const char* a_name, float x, float y, float dx, float dy, int ticks, bool (*a_ok)(const Mover&))
{
	Mover m{ w, { x, y, 100 } };
	for (int i = 0; i < 30; ++i) m.Gravity();
	float start[3] = { m.p[0], m.p[1], m.Feet() };
	for (int i = 0; i < ticks; ++i) {
		m.Move(dx, dy);
		m.Gravity();
	}
	bool ok = a_ok(m);
	printf("%-34s %s  start (%.0f %.0f %.1f) -> end (%.1f %.1f %.1f)\n", a_name, ok ? "ok  " : "FAIL", start[0], start[1], start[2], m.p[0], m.p[1],
		m.Feet());
	return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
	FILE* f = fopen(argc > 1 ? argv[1] : "bin/scan_boxes.txt", "r");
	if (!f) return printf("no box file\n"), 2;
	std::unordered_map<int64_t, Tile> tiles;
	Tile& t = tiles[0];
	if (fscanf(f, "%f %f %f %f %f %f", &t.bounds[0], &t.bounds[1], &t.bounds[2], &t.bounds[3], &t.bounds[4], &t.bounds[5]) != 6) return 2;
	Box b;
	while (fscanf(f, "%f %f %f %f %f %f", &b.mn[0], &b.mn[1], &b.mn[2], &b.mx[0], &b.mx[1], &b.mx[2]) == 6) t.boxes.push_back(b);
	fclose(f);
	Snapshot w(tiles);
	printf("walk_test: %zu boxes\n", w.Boxes());
	int bad = 0;
	// MW2 space: BL2 / 2.54 with y mirrored. Building walls at x 39..236, y -39..-236; door x 220..236, y -142..-220.
	bad += Walk(w, "stands on flat ground", 20, 100, 0, 0, 10, [](const Mover& m) { return std::fabs(m.Feet()) < 1.0f; });
	bad += Walk(w, "climbs the 17-degree hill", 290, 100, 8, 0, 34, [](const Mover& m) { return m.p[0] > 550 && m.Feet() > 70 && m.Feet() < 100; });
	bad += Walk(w, "stopped by the building wall", 150, 100, 0, -8, 30, [](const Mover& m) { return m.p[1] > -25 && m.p[1] < 0; });
	bad += Walk(w, "walks in through the door", 270, -180, -8, 0, 20, [](const Mover& m) { return m.p[0] < 170 && std::fabs(m.Feet()) < 1.0f; });
	bad += Walk(w, "can't walk through the table", 80, -114, 8, 0, 12, [](const Mover& m) { return m.p[0] < 101 - 15 + 1; });  // table at x 98.4; columns sample cell centres (<= 1 cell = 12.6 in error)
	printf("walk_test: %s\n", bad ? "FAILED" : "OK");
	return bad;
}
