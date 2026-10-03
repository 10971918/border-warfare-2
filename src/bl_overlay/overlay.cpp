// Border Warfare 2 - M3: draws MW2's first-person frame (hands, gun, MW2 HUD) over Borderlands 2.
// bw2_overlay.dll (x86), loaded by the BL2 SDK mod with ctypes:
//   bw2_overlay_start()      open the frame mapping, hook IDirect3DDevice9::Present/Reset (d3d9 vtable)
//   bw2_overlay_enable(on)   ask the MW2 guest to capture (FrameHeader::enabled) and draw while on
//   bw2_overlay_hud(...)     BL2 health/shield bars + damage flash (M5) while driving
//   bw2_overlay_stats(buf)   frames drawn / uploaded / errors (for the mod's status line)
// At BL2's Present the newest complete MW2 frame (straight-alpha BGRA, see src/mw_guest/capture.h) is
// uploaded to a dynamic texture and drawn over the whole back buffer with alpha blending; BL2's render
// state is saved and restored around it with a state block.
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../../protocol/bw2_protocol.h"
#include "../common/d3d9hook.h"

#pragma comment(lib, "d3d9.lib")

namespace
{
	using namespace bw2;

	uint8_t*                g_map = nullptr;
	IDirect3DTexture9*      g_tex = nullptr;
	IDirect3DStateBlock9*   g_state = nullptr;
	UINT                    g_tw = 0, g_th = 0;
	uint64_t                g_shownId = 0;
	std::atomic<bool>       g_on{ false };
	std::atomic<uint32_t>   g_drawn{ 0 }, g_uploaded{ 0 }, g_errors{ 0 };

	struct Vertex
	{
		float x, y, z, rhw, u, v;
	};

	// ---- M5 HUD: BL2's shield and health as two bars, bottom left, and a red flash when they drop -----------
	struct ColorVertex
	{
		float x, y, z, rhw;
		DWORD c;
	};
	struct Hud
	{
		float hp = 0, hpMax = 0, sh = 0, shMax = 0;
	};
	Hud   g_hud;
	bool  g_hudOn = false;
	float g_lastTotal = -1.0f;
	DWORD g_hurtAt = 0;
	constexpr DWORD kFlashMs = 450;

	void Rect(IDirect3DDevice9* a_dev, float a_x, float a_y, float a_w, float a_h, DWORD a_argb)
	{
		ColorVertex v[4] = { { a_x - 0.5f, a_y - 0.5f, 0, 1, a_argb }, { a_x + a_w - 0.5f, a_y - 0.5f, 0, 1, a_argb },
			{ a_x - 0.5f, a_y + a_h - 0.5f, 0, 1, a_argb }, { a_x + a_w - 0.5f, a_y + a_h - 0.5f, 0, 1, a_argb } };
		a_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(ColorVertex));
	}

	void Bar(IDirect3DDevice9* a_dev, float a_x, float a_y, float a_w, float a_h, float a_frac, DWORD a_fill)
	{
		a_frac = a_frac < 0 ? 0 : a_frac > 1 ? 1 : a_frac;
		float b = a_h * 0.18f;
		Rect(a_dev, a_x - b, a_y - b, a_w + 2 * b, a_h + 2 * b, 0xB0000000);
		if (a_frac > 0) Rect(a_dev, a_x, a_y, a_w * a_frac, a_h, a_fill);
	}

	// Untextured, alpha-blended (the state set up for the frame quad is reused).
	void DrawHud(IDirect3DDevice9* a_dev, float a_w, float a_h)
	{
		a_dev->SetTexture(0, nullptr);
		a_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		a_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		a_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		a_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		a_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
		DWORD now = GetTickCount();
		if (g_hurtAt && now - g_hurtAt < kFlashMs) {  // red edges, fading out
			DWORD a = static_cast<DWORD>(110.0f * (1.0f - static_cast<float>(now - g_hurtAt) / kFlashMs));
			DWORD c = (a << 24) | 0x00C00000;
			float e = a_h * 0.06f;
			Rect(a_dev, 0, 0, a_w, e, c), Rect(a_dev, 0, a_h - e, a_w, e, c);
			Rect(a_dev, 0, e, e, a_h - 2 * e, c), Rect(a_dev, a_w - e, e, e, a_h - 2 * e, c);
		}
		float x = a_w * 0.03f, bw = a_w * 0.16f, bh = a_h * 0.014f, y = a_h * 0.90f;
		if (g_hud.shMax > 0) Bar(a_dev, x, y - bh * 1.9f, bw, bh, g_hud.sh / g_hud.shMax, 0xE040B8FF);
		bool low = g_hud.hpMax > 0 && g_hud.hp < g_hud.hpMax * 0.25f && (now / 250) % 2;  // low health blinks
		Bar(a_dev, x, y, bw, bh, g_hud.hpMax > 0 ? g_hud.hp / g_hud.hpMax : 0, low ? 0xF0FF6060 : 0xF0D02020);
	}

	proto::FrameHeader* Header() { return reinterpret_cast<proto::FrameHeader*>(g_map); }

	void Release()
	{
		if (g_tex) g_tex->Release(), g_tex = nullptr;
		if (g_state) g_state->Release(), g_state = nullptr;
		g_tw = g_th = 0;
		g_shownId = 0;
	}

	// Copy the newest frame into the texture (when there is a new one). False if there is nothing to draw.
	bool Upload(IDirect3DDevice9* a_dev)
	{
		auto*    hdr = Header();
		auto*    seq = reinterpret_cast<std::atomic<uint32_t>*>(&hdr->seq);
		uint32_t s1 = seq->load(std::memory_order_acquire);
		if (s1 & 1) return g_tex != nullptr;
		uint64_t id = hdr->frameId;
		uint32_t slot = hdr->slot, w = hdr->width, h = hdr->height;
		if (seq->load(std::memory_order_acquire) != s1 || !id || !w || !h || w > proto::kFrameMaxWidth || h > proto::kFrameMaxHeight) return g_tex != nullptr;
		if (id == g_shownId) return true;
		if (w != g_tw || h != g_th || !g_tex) {
			if (g_tex) g_tex->Release(), g_tex = nullptr;
			if (FAILED(a_dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tex, nullptr))) {
				g_errors++;
				return false;
			}
			g_tw = w, g_th = h;
		}
		hdr->reading = slot;  // the guest won't overwrite this slot while we copy
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (hdr->slot != slot && hdr->reading == slot) {
			// the guest switched slots meanwhile: the one we marked may be half written - take the new one next time
			hdr->reading = 0xffffffff;
			return true;
		}
		D3DLOCKED_RECT lr;
		bool           ok = SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD));
		if (ok) {
			const uint8_t* src = g_map + proto::kFrameHeaderBytes + slot * proto::kFrameSlotBytes;
			for (UINT y = 0; y < h; ++y) memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, src + y * w * 4, w * 4);
			g_tex->UnlockRect(0);
			g_shownId = id;
			g_uploaded++;
		}
		hdr->reading = 0xffffffff;
		return ok;
	}

	void Draw(IDirect3DDevice9* a_dev)
	{
		bool frame = Upload(a_dev) && g_tex;
		if (!frame && !g_hudOn) return;
		if (!g_state && FAILED(a_dev->CreateStateBlock(D3DSBT_ALL, &g_state))) {
			g_errors++;
			return;
		}
		g_state->Capture();
		IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr, *oldDs = nullptr;
		if (FAILED(a_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
		a_dev->GetRenderTarget(0, &oldRt);
		a_dev->GetDepthStencilSurface(&oldDs);
		D3DSURFACE_DESC d;
		bb->GetDesc(&d);
		a_dev->SetRenderTarget(0, bb);
		a_dev->SetDepthStencilSurface(nullptr);
		D3DVIEWPORT9 vp{ 0, 0, d.Width, d.Height, 0.0f, 1.0f };
		a_dev->SetViewport(&vp);

		a_dev->SetVertexShader(nullptr);
		a_dev->SetPixelShader(nullptr);
		a_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		a_dev->SetTexture(0, g_tex);
		a_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		a_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		a_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		a_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		a_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		a_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		a_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		a_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		a_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		a_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		a_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		a_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
		a_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		a_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		a_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		a_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		a_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		a_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		a_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		a_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		a_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		a_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		a_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		a_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		a_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		a_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		a_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

		float  w = static_cast<float>(d.Width), h = static_cast<float>(d.Height);
		Vertex v[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 } };
		if (SUCCEEDED(a_dev->BeginScene())) {
			if (frame) a_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
			if (g_hudOn) DrawHud(a_dev, w, h);
			a_dev->EndScene();
			g_drawn++;
		}
		a_dev->SetRenderTarget(0, oldRt ? oldRt : bb);
		a_dev->SetDepthStencilSurface(oldDs);
		if (oldRt) oldRt->Release();
		if (oldDs) oldDs->Release();
		bb->Release();
		g_state->Apply();
	}

	void OnPresent(IDirect3DDevice9* a_dev)
	{
		if (g_on.load(std::memory_order_relaxed) && g_map) Draw(a_dev);
	}

	void OnReset(IDirect3DDevice9*) { Release(); }  // D3DPOOL_DEFAULT texture and the state block go before Reset
}

// 1 = hooked (or already), 0 = failed (see GetLastError-ish codes: 2 mapping, 3 device)
extern "C" __declspec(dllexport) int __cdecl bw2_overlay_start()
{
	if (d3d9hook::g_nDev) return 1;
	HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, proto::kFrameMappingBytes, proto::kFrameMappingName);
	if (!m) return 2;
	g_map = static_cast<uint8_t*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, proto::kFrameMappingBytes));
	if (!g_map) return 2;
	Header()->reading = 0xffffffff;

	d3d9hook::Install(&OnPresent, &OnReset);
	d3d9hook::AdoptGameDevices(GetModuleHandleW(nullptr));
	return d3d9hook::g_nDev > 0 ? 1 : 3;
}

// Diagnostics: does the game draw through the d3d9 vtable we patch at all? Counts BeginScene (41),
// EndScene (42) and DrawIndexedPrimitive (82) through the same vtables as the Present hooks.
namespace probe
{
	using SceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
	using DipFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	d3d9hook::Entry       g_begin[d3d9hook::kMax], g_end[d3d9hook::kMax], g_dip[d3d9hook::kMax];
	int                   g_nb = 0, g_ne = 0, g_nd = 0;
	std::atomic<uint32_t> g_cb{ 0 }, g_ce{ 0 }, g_cd{ 0 };
	HRESULT STDMETHODCALLTYPE Begin(IDirect3DDevice9* d)
	{
		g_cb++;
		return reinterpret_cast<SceneFn>(d3d9hook::Orig(g_begin, g_nb, d))(d);
	}
	HRESULT STDMETHODCALLTYPE End(IDirect3DDevice9* d)
	{
		g_ce++;
		return reinterpret_cast<SceneFn>(d3d9hook::Orig(g_end, g_ne, d))(d);
	}
	HRESULT STDMETHODCALLTYPE Dip(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT pc)
	{
		g_cd++;
		return reinterpret_cast<DipFn>(d3d9hook::Orig(g_dip, g_nd, d))(d, t, b, mi, nv, si, pc);
	}
}

// Patches the probe into every vtable the Present hook found. out: begin, end, dip counts.
extern "C" __declspec(dllexport) void __cdecl bw2_overlay_probe(uint32_t* a_out)
{
	if (!probe::g_nb) {
		for (int i = 0; i < d3d9hook::g_nDev; ++i) {
			void** vt = d3d9hook::g_devPresent[i].vt;
			d3d9hook::Patch(probe::g_begin, probe::g_nb, vt, 41, reinterpret_cast<void*>(&probe::Begin));
			d3d9hook::Patch(probe::g_end, probe::g_ne, vt, 42, reinterpret_cast<void*>(&probe::End));
			d3d9hook::Patch(probe::g_dip, probe::g_nd, vt, 82, reinterpret_cast<void*>(&probe::Dip));
		}
	}
	a_out[0] = probe::g_cb, a_out[1] = probe::g_ce, a_out[2] = probe::g_cd;
}

// Finds the game's own device (its vtable differs from d3d9.dll's: see d3d9hook.h) and hooks it.
// Returns how many new device vtables were hooked; call again if the device may have been recreated.
extern "C" __declspec(dllexport) int __cdecl bw2_overlay_adopt()
{
	return d3d9hook::AdoptGameDevices(GetModuleHandleW(nullptr));
}

extern "C" __declspec(dllexport) void __cdecl bw2_overlay_enable(int a_on)
{
	g_on = a_on != 0;
	if (g_map) Header()->enabled = a_on ? 1u : 0u;
}

// M5 HUD values from the mod each tick (hp/shield and their maxima); a drop in hp+shield flashes the edges red.
extern "C" __declspec(dllexport) void __cdecl bw2_overlay_hud(int a_on, float a_hp, float a_hpMax, float a_sh, float a_shMax)
{
	g_hudOn = a_on != 0;
	float total = a_hp + a_sh;
	if (g_hudOn && g_lastTotal >= 0 && total < g_lastTotal - 0.5f) g_hurtAt = GetTickCount();
	g_lastTotal = g_hudOn ? total : -1.0f;
	g_hud = { a_hp, a_hpMax, a_sh, a_shMax };
}

// Hook-loop guard (d3d9hook.h): out[0] loops broken, out[1] d3d9's own Present known, out[2] swap chain Present known,
// out[3] Reset known.
extern "C" __declspec(dllexport) void __cdecl bw2_overlay_guard(uint32_t* a_out)
{
	a_out[0] = d3d9hook::g_loops, a_out[1] = d3d9hook::g_realDevPresent != nullptr;
	a_out[2] = d3d9hook::g_realScPresent != nullptr, a_out[3] = d3d9hook::g_realDevReset != nullptr;
}

// out[0] drawn, out[1] uploaded, out[2] errors, out[3] last frame id from MW2 (low 32 bits), out[4] BL2 presents,
// out[5] hooked device vtables, out[6] hooked swap chain vtables
extern "C" __declspec(dllexport) void __cdecl bw2_overlay_stats(uint32_t* a_out)
{
	a_out[0] = g_drawn, a_out[1] = g_uploaded, a_out[2] = g_errors;
	a_out[3] = g_map ? static_cast<uint32_t>(Header()->frameId) : 0;
	a_out[4] = d3d9hook::g_presents, a_out[5] = d3d9hook::g_nDev, a_out[6] = d3d9hook::g_nSc, a_out[7] = d3d9hook::g_adopted;
}
