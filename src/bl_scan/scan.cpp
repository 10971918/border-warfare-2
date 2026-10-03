// Border Warfare 2 - native BL2 collision scanner (bw2_scan.dll).
//
// The same algorithm as sdk_mods/bw2/scan.py (the reference; see it for the why), ported to C++ because
// a BL2 trace from Python costs 32-64 us of call overhead and Sanctuary needs ~20k traces per tile.
// Python (sdk_mods/bw2/nativescan.py) drives it through a small C ABI:
//   * in BL2 (x86): traces go to Actor.Trace through the Python SDK's exported process_event, with the
//     parameter offsets Python reads from the UFunction (and UFunction::iNative cleared by Python around
//     bw2_scan_run, as for every numbered native)
//   * in tests (x64): traces go to a callback, so tests/test_native_scan.py can compare it box for box
//     with scan.py on the synthetic world
// Scans are run in time slices (bw2_scan_run(budget)) so BL2's frame time stays bounded.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace
{
	// ---- must match scan.py ----
	constexpr double CELL = 32.0;
	constexpr int    TILE_CELLS = 24;
	constexpr double TILE = CELL * TILE_CELLS;
	constexpr double Z_HALF = 1200.0;
	constexpr int    MAX_LAYERS = 4;
	constexpr double THIN_FLOOR = 24.0;
	constexpr double PROBE_KNEE = 55.0, PROBE_CHEST = 100.0;
	constexpr double WALL_LO = -20.0, WALL_HI = 190.0;
	constexpr double STEP = 3.0;
	constexpr double INSIDE_EPS = 0.6;
	constexpr double EXIT_STEP = 64.0;

	using TraceCb = int(__cdecl*)(void* a_user, const float* a_start, const float* a_end, float* a_hit, float* a_normal);
	using ProcessEventFn = void(__cdecl*)(void* a_object, void* a_function, void* a_params);

	struct Hit
	{
		double p[3];
		double n[3];
	};

	struct Span
	{
		double lo, hi;
		bool   operator<(const Span& o) const { return lo < o.lo || (lo == o.lo && hi < o.hi); }
	};

	struct Scan
	{
		int    tx, ty;
		double x0, y0, zlo, zhi;
		// tracer
		TraceCb        cb = nullptr;
		void*          user = nullptr;
		ProcessEventFn pe = nullptr;
		void*          obj = nullptr;
		void*          func = nullptr;
		int            offHitLoc = 0, offHitNorm = 0, offEnd = 0, offStart = 0, offRet = 0;
		std::vector<uint8_t> params;
		// state
		int                             phase = 0, next = 0;  // phase 0 columns, 1 walls, 2 done
		int                             traces = 0;
		std::vector<std::vector<Span>>  spans;
		std::vector<std::optional<double>> tops;
		std::vector<float>              boxes;

		std::optional<Hit> T(const double a_s[3], const double a_e[3])
		{
			++traces;
			float s[3] = { float(a_s[0]), float(a_s[1]), float(a_s[2]) }, e[3] = { float(a_e[0]), float(a_e[1]), float(a_e[2]) };
			float h[3] = {}, n[3] = {};
			bool  hit = false;
			if (cb) {
				hit = cb(user, s, e, h, n) != 0;
			} else if (pe) {
				std::fill(params.begin(), params.end(), uint8_t(0));
				memcpy(params.data() + offEnd, e, 12);
				memcpy(params.data() + offStart, s, 12);
				pe(obj, func, params.data());
				void* ret = nullptr;
				memcpy(&ret, params.data() + offRet, sizeof(ret));
				if (ret) {
					hit = true;
					memcpy(h, params.data() + offHitLoc, 12);
					memcpy(n, params.data() + offHitNorm, 12);
				}
			}
			if (!hit) return std::nullopt;
			return Hit{ { h[0], h[1], h[2] }, { n[0], n[1], n[2] } };
		}

		bool Inside(double cx, double cy, double z)
		{
			const double s[3] = { cx, cy, z }, e[3] = { cx, cy, z - 1.0 };
			auto         h = T(s, e);
			return h && std::fabs(h->p[2] - z) < INSIDE_EPS;
		}

		// false = reached the bottom of the range (solid all the way down)
		bool ExitBelow(double cx, double cy, double z, double& a_bottom, double& a_free)
		{
			double probe = z;
			for (;;) {
				probe -= EXIT_STEP;
				if (probe <= zlo) {
					a_bottom = zlo;
					return false;
				}
				if (!Inside(cx, cy, probe)) break;
			}
			const double s[3] = { cx, cy, probe }, e[3] = { cx, cy, probe + EXIT_STEP + 1.0 };
			auto         up = T(s, e);
			a_bottom = up ? up->p[2] : probe;
			a_free = probe;
			return true;
		}

		void Column(int i, int j)
		{
			double cx = x0 + (i + 0.5) * CELL, cy = y0 + (j + 0.5) * CELL;
			auto&  sp = spans[j * TILE_CELLS + i];
			std::vector<double>   pending;
			std::optional<double> mainTop;
			double                z = zhi;
			int                   layers = 0;
			while (layers < MAX_LAYERS && z > zlo) {
				const double s[3] = { cx, cy, z }, e[3] = { cx, cy, zlo };
				auto         hit = T(s, e);
				if (!hit) {
					if (layers == 0) sp.push_back({ zlo, z });
					break;
				}
				double hz = hit->p[2];
				++layers;
				double bottom, free;
				if (hz >= z - INSIDE_EPS) {
					bool more = ExitBelow(cx, cy, z, bottom, free);
					sp.push_back({ bottom, z });
					if (!more) break;
					z = free;
					continue;
				}
				if (!mainTop) mainTop = hz;
				if (Inside(cx, cy, hz - STEP)) {
					bool more = ExitBelow(cx, cy, hz - STEP, bottom, free);
					sp.push_back({ bottom, hz });
					if (!more) break;
					z = free;
				} else {
					pending.push_back(hz);
					z = hz - STEP;
				}
			}
			for (size_t k = 0; k < pending.size(); ++k) {
				double top = pending[k];
				bool   have = false;
				double below = 0;
				for (const auto& s : sp)
					if (s.hi < top && (!have || s.hi > below)) below = s.hi, have = true;
				if (k + 1 < pending.size() && (!have || pending[k + 1] > below)) below = pending[k + 1], have = true;
				if (!have) {
					sp.push_back({ zlo, top });
					continue;
				}
				const double s[3] = { cx, cy, below + STEP }, e[3] = { cx, cy, top - STEP };
				auto         up = T(s, e);
				double       bottom = up ? up->p[2] : top - THIN_FLOOR;
				sp.push_back({ std::max(bottom, below), top });
			}
			tops[j * TILE_CELLS + i] = mainTop;
		}

		void Walls(int i, int j)
		{
			auto top = tops[j * TILE_CELLS + i];
			if (!top) return;
			double cx = x0 + (i + 0.5) * CELL, cy = y0 + (j + 0.5) * CELL;
			static const int knee[2][2] = { { 1, 0 }, { 0, 1 } };
			static const int chest[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
			for (int pass = 0; pass < 2; ++pass) {
				double z = *top + (pass ? PROBE_CHEST : PROBE_KNEE);
				if (z >= zhi) continue;
				int n = pass ? 4 : 2;
				for (int d = 0; d < n; ++d) {
					int          dx = pass ? chest[d][0] : knee[d][0], dy = pass ? chest[d][1] : knee[d][1];
					const double s[3] = { cx, cy, z }, e[3] = { cx + dx * CELL, cy + dy * CELL, z };
					auto         hit = T(s, e);
					if (!hit) continue;
					double bx = hit->p[0] - hit->n[0] * 4.0, by = hit->p[1] - hit->n[1] * 4.0;
					int    ci = int(std::floor((bx - x0) / CELL)), cj = int(std::floor((by - y0) / CELL));
					if (ci >= 0 && ci < TILE_CELLS && cj >= 0 && cj < TILE_CELLS)
						spans[cj * TILE_CELLS + ci].push_back({ *top + WALL_LO, std::min(zhi, *top + WALL_HI) });
				}
			}
		}

		void BuildBoxes()
		{
			const int n = TILE_CELLS;
			std::vector<std::vector<Span>> merged(n * n);
			for (int idx = 0; idx < n * n; ++idx) {
				auto sp = spans[idx];
				std::sort(sp.begin(), sp.end());
				auto& out = merged[idx];
				for (auto s : sp) {
					double lo = std::nearbyint(std::max(s.lo, zlo)), hi = std::nearbyint(std::min(s.hi, zhi));  // round half to even, like Python
					if (hi <= lo) continue;
					if (!out.empty() && lo <= out.back().hi + 1) {
						out.back().hi = std::max(out.back().hi, hi);
					} else {
						out.push_back({ lo, hi });
					}
				}
			}
			auto same = [](const std::vector<Span>& a, const std::vector<Span>& b) {
				if (a.size() != b.size()) return false;
				for (size_t k = 0; k < a.size(); ++k)
					if (a[k].lo != b[k].lo || a[k].hi != b[k].hi) return false;
				return true;
			};
			for (int j = 0; j < n; ++j) {
				int i = 0;
				while (i < n) {
					const auto& key = merged[j * n + i];
					int         k = i + 1;
					while (k < n && same(merged[j * n + k], key)) ++k;
					for (const auto& s : key) {
						float b[6] = { float(x0 + i * CELL), float(y0 + j * CELL), float(s.lo), float(x0 + k * CELL), float(y0 + (j + 1) * CELL), float(s.hi) };
						boxes.insert(boxes.end(), b, b + 6);
					}
					i = k;
				}
			}
		}
	};
}

extern "C" __declspec(dllexport) void* __cdecl bw2_scan_new(int a_tx, int a_ty, double a_zref)
{
	auto* s = new Scan();
	s->tx = a_tx, s->ty = a_ty;
	s->x0 = a_tx * TILE, s->y0 = a_ty * TILE;
	s->zlo = a_zref - Z_HALF, s->zhi = a_zref + Z_HALF;
	s->spans.resize(TILE_CELLS * TILE_CELLS);
	s->tops.resize(TILE_CELLS * TILE_CELLS);
	return s;
}

extern "C" __declspec(dllexport) void __cdecl bw2_scan_set_callback(void* a_scan, TraceCb a_cb, void* a_user)
{
	auto* s = static_cast<Scan*>(a_scan);
	s->cb = a_cb, s->user = a_user;
}

// In BL2: trace through the SDK's process_event. Returns 0 if unrealsdk.dll's export is missing.
extern "C" __declspec(dllexport) int __cdecl bw2_scan_set_unreal(void* a_scan, void* a_object, void* a_func, int a_paramsSize, int a_offHitLoc,
	int a_offHitNorm, int a_offEnd, int a_offStart, int a_offRet)
{
	auto*   s = static_cast<Scan*>(a_scan);
	HMODULE sdk = GetModuleHandleW(L"unrealsdk.dll");
	s->pe = sdk ? reinterpret_cast<ProcessEventFn>(GetProcAddress(sdk, "_unrealsdk_export__process_event")) : nullptr;
	s->obj = a_object, s->func = a_func;
	s->params.assign(static_cast<size_t>(a_paramsSize), 0);
	s->offHitLoc = a_offHitLoc, s->offHitNorm = a_offHitNorm, s->offEnd = a_offEnd, s->offStart = a_offStart, s->offRet = a_offRet;
	return s->pe != nullptr;
}

// Scans for about a_budgetMs (whole columns at a time). Returns 1 when the tile is done.
extern "C" __declspec(dllexport) int __cdecl bw2_scan_run(void* a_scan, double a_budgetMs)
{
	auto* s = static_cast<Scan*>(a_scan);
	auto  end = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<int64_t>(a_budgetMs * 1000.0));
	const int n = TILE_CELLS * TILE_CELLS;
	while (s->phase < 2) {
		if (s->phase == 0) {
			s->Column(s->next % TILE_CELLS, s->next / TILE_CELLS);
		} else {
			s->Walls(s->next % TILE_CELLS, s->next / TILE_CELLS);
		}
		if (++s->next == n) {
			s->next = 0;
			if (++s->phase == 2) s->BuildBoxes();
		}
		if (std::chrono::steady_clock::now() >= end) break;
	}
	return s->phase == 2;
}

extern "C" __declspec(dllexport) int __cdecl bw2_scan_traces(void* a_scan) { return static_cast<Scan*>(a_scan)->traces; }

// Copies up to a_max boxes (6 floats each: min xyz, max xyz, BL2 space); returns the total box count.
extern "C" __declspec(dllexport) int __cdecl bw2_scan_boxes(void* a_scan, float* a_out, int a_max)
{
	auto* s = static_cast<Scan*>(a_scan);
	int   count = static_cast<int>(s->boxes.size() / 6);
	if (a_out) memcpy(a_out, s->boxes.data(), sizeof(float) * 6 * std::min(count, a_max));
	return count;
}

extern "C" __declspec(dllexport) void __cdecl bw2_scan_free(void* a_scan) { delete static_cast<Scan*>(a_scan); }
