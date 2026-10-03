// Border Warfare 2 - M3: MW2's first-person frame for BL2 (guest side, x64).
//
// While driving, MW2's player stands in the void below its own level and MW2 is set to clear to magenta
// with the world, sky, fog, glow, DOF and HUD off (MakeSafe), so its frame is just the hands and gun.
// Every presented frame (up to kMaxFps) is copied to system memory, the magenta becomes alpha (soft edge,
// spill removed) and the BGRA pixels go to the free slot of the frame mapping (protocol FrameHeader).
//
// Hook: MW2's own present function (see PresentHook); no D3D vtables are patched.
#pragma once

#include <d3d9.h>

#include <atomic>
#include <cstdint>

#include "../../protocol/bw2_protocol.h"
#include "../common/link.h"
#include "../common/util.h"

#pragma comment(lib, "d3d9.lib")

namespace bw2::capture
{
	constexpr DWORD kMinFrameMs = 13;  // ~75 fps cap: MW2 waits on the GPU copy, no need to do it more often


	inline uint8_t*             g_map = nullptr;
	inline IDirect3DSurface9*   g_sys = nullptr;
	inline UINT                 g_w = 0, g_h = 0;
	inline D3DFORMAT            g_fmt = D3DFMT_UNKNOWN;
	inline DWORD                g_last = 0;
	inline std::atomic<uint64_t> g_frames{ 0 };
	inline std::atomic<uint32_t> g_errors{ 0 };

	inline proto::FrameHeader* Header() { return reinterpret_cast<proto::FrameHeader*>(g_map); }
	inline uint8_t*            Slot(uint32_t a_i) { return g_map + proto::kFrameHeaderBytes + a_i * proto::kFrameSlotBytes; }

	inline void Release()
	{
		if (g_sys) g_sys->Release(), g_sys = nullptr;
		g_w = g_h = 0;
	}

	// Magenta key -> alpha. m = how magenta the pixel is (min(r, b) - g): >= 110 fully keyed, <= 30 opaque,
	// in between an antialiased edge; the magenta share is taken out of the colour so edges don't glow pink.
	inline void KeyRow(const uint8_t* a_src, uint8_t* a_dst, UINT a_w)
	{
		for (UINT x = 0; x < a_w; ++x, a_src += 4, a_dst += 4) {
			int b = a_src[0], g = a_src[1], r = a_src[2];
			int m = (r < b ? r : b) - g;
			int alpha = m >= 110 ? 0 : m <= 30 ? 255 : 255 - (m - 30) * 255 / 80;
			if (alpha < 255 && alpha > 0) {  // despill: remove the magenta that bled into the edge
				int spill = (255 - alpha) * m / 255;
				r = r - spill < 0 ? 0 : r - spill;
				b = b - spill < 0 ? 0 : b - spill;
			}
			a_dst[0] = static_cast<uint8_t>(b);
			a_dst[1] = static_cast<uint8_t>(g);
			a_dst[2] = static_cast<uint8_t>(r);
			a_dst[3] = static_cast<uint8_t>(alpha);
		}
	}

	// Contingency's falling snow is drawn with the gun: faint, isolated dots. A see-through pixel (alpha below
	// kSolid) with no solid neighbour is such a dot and is removed. The gun, the muzzle flash and the HUD are
	// solid, so their soft edges (which always touch a solid pixel) stay.
	constexpr uint8_t kSolid = 200;

	inline void Despeckle(uint8_t* a_px, UINT a_w, UINT a_h)
	{
		for (UINT y = 1; y + 1 < a_h; ++y) {
			uint8_t* row = a_px + y * a_w * 4;
			for (UINT x = 1; x + 1 < a_w; ++x) {
				uint8_t a = row[x * 4 + 3];
				if (a == 0 || a >= kSolid) continue;
				int solid = 0;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
						if ((dx || dy) && row[(static_cast<int>(dy) * static_cast<int>(a_w) + static_cast<int>(x) + dx) * 4 + 3] >= kSolid) ++solid;
				if (!solid) row[x * 4 + 3] = 0;
			}
		}
	}

	inline void Capture(IDirect3DDevice9* a_dev)
	{
		auto* hdr = Header();
		if (!hdr->enabled) return;
		DWORD now = GetTickCount();
		if (now - g_last < kMinFrameMs) return;
		g_last = now;

		// Back buffer straight to system memory (it isn't multisampled in MW2): no D3DPOOL_DEFAULT surface of
		// ours exists, so MW2's device Reset never needs to know about us.
		IDirect3DSurface9* bb = nullptr;
		if (FAILED(a_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
		D3DSURFACE_DESC d;
		bb->GetDesc(&d);
		if (d.Width > proto::kFrameMaxWidth || d.Height > proto::kFrameMaxHeight || d.MultiSampleType != D3DMULTISAMPLE_NONE ||
			(d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8)) {
			bb->Release();
			if (!g_errors++) Log("capture: back buffer %ux%u format %d multisample %d not supported", d.Width, d.Height, d.Format, d.MultiSampleType);
			return;
		}
		if (d.Width != g_w || d.Height != g_h || d.Format != g_fmt || !g_sys) {
			Release();
			if (FAILED(a_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &g_sys, nullptr))) {
				bb->Release();
				g_errors++;
				return;
			}
			g_w = d.Width, g_h = d.Height, g_fmt = d.Format;
			Log("capture: %ux%u (back buffer format %d)", g_w, g_h, d.Format);
		}
		bool ok = SUCCEEDED(a_dev->GetRenderTargetData(bb, g_sys));
		bb->Release();
		if (!ok) {
			g_errors++;
			return;
		}
		D3DLOCKED_RECT lr;
		if (FAILED(g_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
		static bool first = true;
		if (first) first = false, Log("capture: first frame from MW2's Present");
		uint32_t slot = (hdr->slot ^ 1u) & 1u;
		if ((hdr->reading & 1u) == slot && hdr->reading <= 1u) slot ^= 1u;  // never write the slot the host is copying
		uint8_t* dst = Slot(slot);
		for (UINT y = 0; y < g_h; ++y) KeyRow(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch, dst + y * g_w * 4, g_w);
		g_sys->UnlockRect();
		Despeckle(dst, g_w, g_h);

		LARGE_INTEGER q;
		QueryPerformanceCounter(&q);
		auto* seq = reinterpret_cast<std::atomic<uint32_t>*>(&hdr->seq);
		uint32_t s = seq->load(std::memory_order_relaxed);
		seq->store(s | 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		hdr->slot = slot, hdr->width = g_w, hdr->height = g_h, hdr->qpc = q.QuadPart;
		hdr->frameId = ++g_frames;
		std::atomic_thread_fence(std::memory_order_release);
		seq->store((s | 1) + 1, std::memory_order_release);
	}

	// ---- where the frame comes from --------------------------------------------------------------------------
	// MW2's present function (exe+0x45060, "Direct3DDevice9::Present failed: %s"; bit 2 of its one argument =
	// present the window via its swap chain) is reached through two direct calls. Those calls are re-pointed at
	// a stub next to the exe that jumps to PresentHook, which copies the finished frame from MW2's device
	// (global at exe+0x410fd20) and then presents as normal. No D3D vtable is touched: patching Present in the
	// device vtables fought the Steam overlay's own Present hook after a device reset (stack overflow in
	// gameoverlayrenderer64.dll, 2026-10-03).
	constexpr uintptr_t kPresentFn = 0x45060, kDeviceGlobal = 0x410fd20;
	constexpr uintptr_t kPresentCalls[] = { 0xc77d, 0x45757 };
	using PresentFn = uint64_t (*)(uint64_t);
	inline uintptr_t g_exeBase = 0;
	inline std::atomic<uint32_t> g_presents{ 0 };

	inline void CaptureGuarded(IDirect3DDevice9* a_dev)
	{
		__try {
			Capture(a_dev);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			g_errors++;
		}
	}

	inline uint64_t PresentHook(uint64_t a_flags)
	{
		g_presents++;
		if ((a_flags & 2) && g_map) {
			auto* dev = *reinterpret_cast<IDirect3DDevice9**>(g_exeBase + kDeviceGlobal);
			if (dev) CaptureGuarded(dev);
		}
		return reinterpret_cast<PresentFn>(g_exeBase + kPresentFn)(a_flags);
	}

	// A jmp [rip+0] stub within rel32 reach of the exe (searched downwards from its base).
	inline uint8_t* NearStub(uintptr_t a_base, void* a_to)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		for (uintptr_t at = (a_base - 0x10000) & ~static_cast<uintptr_t>(si.dwAllocationGranularity - 1); a_base - at < 0x7ff00000;
			 at -= si.dwAllocationGranularity) {
			if (auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
				const uint8_t jmp[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
				memcpy(p, jmp, 6);
				memcpy(p + 6, &a_to, 8);
				FlushInstructionCache(GetCurrentProcess(), p, 14);
				return p;
			}
		}
		return nullptr;
	}

	// Opens the frame mapping and re-points MW2's present calls (each must still be "call exe+0x45060").
	inline bool Install()
	{
		g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		for (uintptr_t c : kPresentCalls) {
			auto* at = reinterpret_cast<uint8_t*>(g_exeBase + c);
			int32_t rel;
			memcpy(&rel, at + 1, 4);
			if (at[0] != 0xE8 || c + 5 + rel != kPresentFn) {
				Log("capture: present call at exe+0x%llx not found (other build?) - capture off", static_cast<unsigned long long>(c));
				return false;
			}
		}
		HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, proto::kFrameMappingBytes, proto::kFrameMappingName);
		if (!m) return false;
		g_map = static_cast<uint8_t*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, proto::kFrameMappingBytes));
		if (!g_map) return false;
		uint8_t* stub = NearStub(g_exeBase, reinterpret_cast<void*>(&PresentHook));
		if (!stub) {
			Log("capture: no memory near the exe for the present stub - capture off");
			return false;
		}
		for (uintptr_t c : kPresentCalls) {
			auto*   at = reinterpret_cast<uint8_t*>(g_exeBase + c);
			int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(at + 5));
			DWORD   old;
			VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
			memcpy(at + 1, &rel, 4);
			VirtualProtect(at, 5, old, &old);
			FlushInstructionCache(GetCurrentProcess(), at, 5);
		}
		Log("capture: MW2's present calls re-pointed (frame mapping %u MB)", proto::kFrameMappingBytes >> 20);
		return true;
	}
}
