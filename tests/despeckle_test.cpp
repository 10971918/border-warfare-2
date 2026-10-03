// Border Warfare 2 - capture::Despeckle test (x64): snow dots go, the gun's soft edges stay.
#include <cstdio>
#include <vector>

#include "../src/mw_guest/capture.h"

namespace
{
	constexpr UINT W = 64, H = 32;
	uint8_t& A(std::vector<uint8_t>& a_px, UINT a_x, UINT a_y) { return a_px[(a_y * W + a_x) * 4 + 3]; }
}

int main()
{
	std::vector<uint8_t> px(W * H * 4, 0);
	// a "gun": a solid block with a soft (half-transparent) border
	for (UINT y = 10; y < 22; ++y)
		for (UINT x = 30; x < 50; ++x) A(px, x, y) = (y == 10 || y == 21 || x == 30 || x == 49) ? 120 : 255;
	// snow: a faint single dot, a faint 2x2 dot
	A(px, 5, 5) = 90;
	A(px, 12, 20) = A(px, 13, 20) = A(px, 12, 21) = A(px, 13, 21) = 70;
	bw2::capture::Despeckle(px.data(), W, H);

	int fail = 0;
	if (A(px, 5, 5)) std::printf("FAIL: single snow dot kept\n"), ++fail;
	if (A(px, 12, 20) || A(px, 13, 21)) std::printf("FAIL: 2x2 snow dot kept\n"), ++fail;
	for (UINT y = 10; y < 22; ++y)
		for (UINT x = 30; x < 50; ++x)
			if (!A(px, x, y)) std::printf("FAIL: gun pixel %u,%u removed\n", x, y), ++fail;
	if (fail) return 1;
	std::printf("despeckle_test OK\n");
	return 0;
}
