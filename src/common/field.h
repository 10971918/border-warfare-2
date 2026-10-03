// Border Warfare 2: BL2 collision as MW2 sees it (M2).
//
// The host scans Borderlands 2 around the player and streams solid axis-aligned boxes per tile (guest
// space, inches). The guest keeps them in an immutable Snapshot (rebuilt off the game thread, swapped
// atomically) and answers Pmove's box traces against it with the classic Quake brush clip, the same
// entering/leaving-plane logic and SURFACE_CLIP_EPSILON the IW engine uses, so MW2's own movement
// code (step-up, slide, ground checks, jumps, mantles) runs unchanged on BL2's geometry.
// Space no tile covers yet answers "blocked" (the player holds still instead of falling).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace bw2::field
{
	inline constexpr float kSurfEps = 0.125f;  // SURFACE_CLIP_EPSILON
	inline constexpr float kBucket = 64.0f;    // inches per spatial bucket (x, y)
	inline constexpr float kWalkableZ = 0.7f;  // normal.z at or above this is ground you can stand on

	struct Box
	{
		float mn[3];
		float mx[3];
	};

	struct Tile
	{
		int32_t          tx = 0, ty = 0;
		float            bounds[6] = {};  // min xyz, max xyz: the space this tile is authoritative for
		std::vector<Box> boxes;
	};

	struct Result
	{
		float fraction = 1.0f;
		float normal[3] = { 0, 0, 0 };
		bool  startSolid = false;
		bool  allSolid = false;
		bool  hold = false;  // not covered by any tile: answered blocked
	};

	class Snapshot
	{
	public:
		explicit Snapshot(const std::unordered_map<int64_t, Tile>& a_tiles)
		{
			for (const auto& [key, t] : a_tiles) {
				_tiles.push_back({ { t.bounds[0], t.bounds[1], t.bounds[2] }, { t.bounds[3], t.bounds[4], t.bounds[5] } });
				for (const auto& b : t.boxes) {
					uint32_t id = static_cast<uint32_t>(_boxes.size());
					_boxes.push_back(b);
					for (int64_t bx = Cell(b.mn[0]); bx <= Cell(b.mx[0]); ++bx)
						for (int64_t by = Cell(b.mn[1]); by <= Cell(b.mx[1]); ++by) _buckets[Key(bx, by)].push_back(id);
				}
			}
		}

		// Boxes overlapping the given xy rectangle (debugging).
		std::vector<Box> Near(float a_x0, float a_y0, float a_x1, float a_y1) const
		{
			std::vector<Box> out;
			for (const auto& b : _boxes)
				if (b.mn[0] <= a_x1 && b.mx[0] >= a_x0 && b.mn[1] <= a_y1 && b.mx[1] >= a_y0) out.push_back(b);
			return out;
		}

		size_t Tiles() const { return _tiles.size(); }
		size_t Boxes() const { return _boxes.size(); }

		// True when every xy corner of [mn, mx] (and its z range) lies inside some tile.
		bool Covered(const float a_mn[3], const float a_mx[3]) const
		{
			const float xs[2] = { a_mn[0], a_mx[0] }, ys[2] = { a_mn[1], a_mx[1] };
			for (float x : xs)
				for (float y : ys) {
					bool in = false;
					for (const auto& t : _tiles) {
						if (x >= t.mn[0] && x <= t.mx[0] && y >= t.mn[1] && y <= t.mx[1] && a_mn[2] >= t.mn[2] && a_mx[2] <= t.mx[2]) {
							in = true;
							break;
						}
					}
					if (!in) return false;
				}
			return true;
		}

		// Sweep a box with half extents a_half whose centre moves a_start -> a_end. Read-only, so any thread
		// may call it. A box spanning several buckets may be clipped twice; that changes nothing.
		// a_escape: ignore boxes the start is already inside (lets a player embedded too deep to lift out walk free).
		Result Trace(const float a_start[3], const float a_end[3], const float a_half[3], bool a_escape = false) const
		{
			Result r;
			float  smn[3], smx[3];
			for (int i = 0; i < 3; ++i) {
				smn[i] = std::min(a_start[i], a_end[i]) - a_half[i];
				smx[i] = std::max(a_start[i], a_end[i]) + a_half[i];
			}
			if (!Covered(smn, smx)) {
				r.fraction = 0.0f;
				r.normal[2] = 1.0f;
				r.hold = true;
				return r;
			}
			for (int64_t bx = Cell(smn[0] - kSurfEps); bx <= Cell(smx[0] + kSurfEps); ++bx) {
				for (int64_t by = Cell(smn[1] - kSurfEps); by <= Cell(smx[1] + kSurfEps); ++by) {
					auto it = _buckets.find(Key(bx, by));
					if (it == _buckets.end()) continue;
					for (uint32_t id : it->second) {
						const Box& b = _boxes[id];
						if (b.mx[2] < smn[2] - kSurfEps || b.mn[2] > smx[2] + kSurfEps || b.mx[0] < smn[0] - kSurfEps ||
							b.mn[0] > smx[0] + kSurfEps || b.mx[1] < smn[1] - kSurfEps || b.mn[1] > smx[1] + kSurfEps) {
							continue;
						}
						if (a_escape && Inside(b, a_start, a_half)) continue;
						Clip(b, a_start, a_end, a_half, r);
						if (r.allSolid) return r;
					}
				}
			}
			return r;
		}

		// How far a box (centre, half extents) has to go straight up to be out of every solid, or -1 when
		// that is more than a_maxLift. MW2 rounds the player like a capsule at ledges and our boxes are
		// square, so it can settle a fraction of an inch into the next cell's floor; Pmove then sees
		// "all solid" and freezes. The guest lifts it out by this much (step-sized penetrations only).
		float LiftOut(const float a_c[3], const float a_h[3], float a_maxLift) const
		{
			float dz = 0.0f;
			for (int guard = 0; guard < 64; ++guard) {
				float c[3] = { a_c[0], a_c[1], a_c[2] + dz };
				const Box* in = nullptr;
				auto it = _buckets.find(Key(Cell(c[0]), Cell(c[1])));
				for (int64_t bx = Cell(c[0] - a_h[0]); bx <= Cell(c[0] + a_h[0]) && !in; ++bx) {
					for (int64_t by = Cell(c[1] - a_h[1]); by <= Cell(c[1] + a_h[1]) && !in; ++by) {
						it = _buckets.find(Key(bx, by));
						if (it == _buckets.end()) continue;
						for (uint32_t id : it->second) {
							const Box& b = _boxes[id];
							bool inside = true;
							for (int a = 0; a < 3 && inside; ++a) inside = c[a] > b.mn[a] - a_h[a] && c[a] < b.mx[a] + a_h[a];
							if (inside) {
								in = &b;
								break;
							}
						}
					}
				}
				if (!in) return dz;
				dz = in->mx[2] + a_h[2] - a_c[2] + 0.25f;  // just above that box's top
				if (dz > a_maxLift) return -1.0f;
			}
			return -1.0f;
		}

		// Nearest free spot for a box embedded in solid: straight up first (LiftOut), then rings of 16 directions
		// out to a_maxRadius, each with a step-sized lift. Writes the offset; false if nothing nearby is free.
		bool FindFree(const float a_c[3], const float a_h[3], float a_maxRadius, float a_out[3]) const
		{
			float lift = LiftOut(a_c, a_h, 24.0f);
			if (lift >= 0.0f) {
				a_out[0] = a_out[1] = 0.0f, a_out[2] = lift;
				return true;
			}
			for (float r = 6.0f; r <= a_maxRadius; r += 6.0f) {
				for (int k = 0; k < 16; ++k) {
					float a = k * 3.14159265f / 8.0f;
					float c[3] = { a_c[0] + r * std::cos(a), a_c[1] + r * std::sin(a), a_c[2] };
					float l = LiftOut(c, a_h, 24.0f);
					if (l >= 0.0f) {
						a_out[0] = c[0] - a_c[0], a_out[1] = c[1] - a_c[1], a_out[2] = l;
						return true;
					}
				}
			}
			return false;
		}

		static bool Inside(const Box& a_b, const float a_c[3], const float a_h[3])
		{
			for (int a = 0; a < 3; ++a)
				if (a_c[a] <= a_b.mn[a] - a_h[a] || a_c[a] >= a_b.mx[a] + a_h[a]) return false;
			return true;
		}

		// Quake's CM_ClipBoxToBrush for an axis-aligned box expanded by the moving box's half extents.
		static void Clip(const Box& a_b, const float a_s[3], const float a_e[3], const float a_h[3], Result& a_r)
		{
			float enterFrac = -1.0f, leaveFrac = 1.0f;
			int   clipAxis = -1;
			float clipSign = 0.0f;
			bool  startOut = false, getOut = false;
			for (int axis = 0; axis < 3; ++axis) {
				for (int side = 0; side < 2; ++side) {
					float sign = side ? -1.0f : 1.0f;  // +axis face, then -axis face
					float dist = side ? -(a_b.mn[axis] - a_h[axis]) : (a_b.mx[axis] + a_h[axis]);
					float d1 = sign * a_s[axis] - dist;
					float d2 = sign * a_e[axis] - dist;
					if (d2 > 0) getOut = true;
					if (d1 > 0) startOut = true;
					if (d1 > 0 && (d2 >= kSurfEps || d2 >= d1)) return;  // entirely in front of this face
					if (d1 <= 0 && d2 <= 0) continue;                      // entirely behind it
					if (d1 > d2) {                                         // entering
						float f = (d1 - kSurfEps) / (d1 - d2);
						if (f < 0) f = 0;
						if (f > enterFrac) enterFrac = f, clipAxis = axis, clipSign = sign;
					} else {                                               // leaving
						float f = (d1 + kSurfEps) / (d1 - d2);
						if (f > 1) f = 1;
						if (f < leaveFrac) leaveFrac = f;
					}
				}
			}
			if (!startOut) {
				a_r.startSolid = true;
				if (!getOut) {
					a_r.allSolid = true;
					a_r.fraction = 0.0f;
				}
				return;
			}
			if (enterFrac < leaveFrac && enterFrac > -1.0f && enterFrac < a_r.fraction && clipAxis >= 0) {
				a_r.fraction = enterFrac < 0 ? 0 : enterFrac;
				a_r.normal[0] = a_r.normal[1] = a_r.normal[2] = 0.0f;
				a_r.normal[clipAxis] = clipSign;
			}
		}

	private:
		struct Region
		{
			float mn[3], mx[3];
		};

		static int64_t Cell(float a_v) { return static_cast<int64_t>(std::floor(a_v / kBucket)); }
		static int64_t Key(int64_t a_x, int64_t a_y) { return (a_x << 32) ^ (a_y & 0xffffffff); }

		std::vector<Region>                             _tiles;
		std::vector<Box>                                _boxes;
		std::unordered_map<int64_t, std::vector<uint32_t>> _buckets;
	};
}
