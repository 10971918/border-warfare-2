// Border Warfare 2: catch a game's D3D9 presents.
//
// The game's device is found in its exe's data (AdoptGameDevices) and Present (device slot 17, swap chain
// slot 3) and Reset (device slot 16) are patched in that device's own vtable; the original is remembered per
// vtable. Hooks in throwaway devices never fire (each device has its own vtable copy here), so Install only
// learns d3d9.dll's own functions from one, for the re-entry guard. Callbacks get the device that is presenting.
#pragma once

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>

#pragma comment(lib, "d3d9.lib")

namespace bw2::d3d9hook
{
	using DevPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
	using ScPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
	using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
	using Callback = void (*)(IDirect3DDevice9*);

	constexpr int kDevReset = 16, kDevPresent = 17, kScPresent = 3, kMax = 16;

	struct Entry
	{
		void** vt = nullptr;
		void*  orig = nullptr;
	};

	inline Entry                 g_devPresent[kMax], g_scPresent[kMax], g_devReset[kMax];
	inline int                   g_nDev = 0, g_nSc = 0, g_nReset = 0;
	inline Callback              g_onPresent = nullptr, g_onReset = nullptr;
	inline std::atomic<uint32_t> g_presents{ 0 };

	inline void* Orig(Entry* a_list, int a_n, void* a_self)
	{
		void** vt = *reinterpret_cast<void***>(a_self);
		for (int i = 0; i < a_n; ++i)
			if (a_list[i].vt == vt) return a_list[i].orig;
		return nullptr;
	}

	// ---- re-entry guard -----------------------------------------------------------------------------------------
	// After a device reset the Steam overlay re-hooks Present on top of ours and remembers *our* hook as the
	// function to call next, while we still call *its* old hook: the two call each other until the stack runs
	// out (it crashed MW2 that way: 0xc00000fd in gameoverlayrenderer64.dll, 2026-10-03). So when one of our hooks
	// is entered again on the same thread, that is the loop: go straight to d3d9.dll's own function instead
	// (remembered when a patched slot still pointed into d3d9.dll), or skip this one present if we never saw it.
	inline thread_local int g_depth = 0;
	inline void*            g_realDevPresent = nullptr;
	inline void*            g_realScPresent = nullptr;
	inline void*            g_realDevReset = nullptr;
	inline std::atomic<uint32_t> g_loops{ 0 };

	struct Depth
	{
		Depth() { ++g_depth; }
		~Depth() { --g_depth; }
	};

	inline HRESULT STDMETHODCALLTYPE DevPresent(IDirect3DDevice9* a_dev, const RECT* a_s, const RECT* a_d, HWND a_w, const RGNDATA* a_r)
	{
		if (g_depth) {
			g_loops++;
			return g_realDevPresent ? reinterpret_cast<DevPresentFn>(g_realDevPresent)(a_dev, a_s, a_d, a_w, a_r) : D3D_OK;
		}
		Depth guard;
		g_presents++;
		if (g_onPresent) g_onPresent(a_dev);
		return reinterpret_cast<DevPresentFn>(Orig(g_devPresent, g_nDev, a_dev))(a_dev, a_s, a_d, a_w, a_r);
	}

	inline HRESULT STDMETHODCALLTYPE ScPresent(IDirect3DSwapChain9* a_sc, const RECT* a_s, const RECT* a_d, HWND a_w, const RGNDATA* a_r, DWORD a_f)
	{
		if (g_depth) {
			g_loops++;
			return g_realScPresent ? reinterpret_cast<ScPresentFn>(g_realScPresent)(a_sc, a_s, a_d, a_w, a_r, a_f) : D3D_OK;
		}
		Depth guard;
		g_presents++;
		IDirect3DDevice9* dev = nullptr;
		if (g_onPresent && SUCCEEDED(a_sc->GetDevice(&dev)) && dev) {
			g_onPresent(dev);
			dev->Release();
		}
		return reinterpret_cast<ScPresentFn>(Orig(g_scPresent, g_nSc, a_sc))(a_sc, a_s, a_d, a_w, a_r, a_f);
	}

	inline HRESULT STDMETHODCALLTYPE DevReset(IDirect3DDevice9* a_dev, D3DPRESENT_PARAMETERS* a_pp)
	{
		if (g_depth) {
			g_loops++;
			return g_realDevReset ? reinterpret_cast<ResetFn>(g_realDevReset)(a_dev, a_pp) : D3DERR_INVALIDCALL;
		}
		Depth guard;
		if (g_onReset) g_onReset(a_dev);
		return reinterpret_cast<ResetFn>(Orig(g_devReset, g_nReset, a_dev))(a_dev, a_pp);
	}

	inline bool InD3d9(const void* a_p)
	{
		auto d3d = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"d3d9.dll"));
		if (!d3d) return false;
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(d3d + reinterpret_cast<IMAGE_DOS_HEADER*>(d3d)->e_lfanew);
		auto  p = reinterpret_cast<uintptr_t>(a_p);
		return p >= d3d && p < d3d + nt->OptionalHeader.SizeOfImage;
	}

	inline void Patch(Entry* a_list, int& a_n, void** a_vt, int a_slot, void* a_hook)
	{
		for (int i = 0; i < a_n; ++i)
			if (a_list[i].vt == a_vt) return;  // this class is done already
		if (a_n >= kMax || a_vt[a_slot] == a_hook) return;
		DWORD old;
		// vtables may share a page with code (x64 d3d9.dll, overlay copies): never drop execute while patching
		if (!VirtualProtect(&a_vt[a_slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
		a_list[a_n].vt = a_vt;
		a_list[a_n].orig = a_vt[a_slot];
		if (InD3d9(a_vt[a_slot])) {  // d3d9.dll's own function: the way out of a hook loop (see DevPresent)
			void** real = a_list == g_devPresent ? &g_realDevPresent : a_list == g_scPresent ? &g_realScPresent : a_list == g_devReset ? &g_realDevReset : nullptr;
			if (real && !*real) *real = a_vt[a_slot];
		}
		std::atomic_thread_fence(std::memory_order_seq_cst);
		++a_n;  // the entry is visible before the pointer that needs it
		InterlockedExchangePointer(&a_vt[a_slot], a_hook);
		VirtualProtect(&a_vt[a_slot], sizeof(void*), old, &old);
	}

	// ---- the game's own device ---------------------------------------------------------------------------------
	// In both games the device in use does NOT have d3d9.dll's vtable (BL2: no BeginScene/EndScene/draw call ever
	// went through it - the Steam overlay, loaded in both, gives the device its own vtable copy). So we find the
	// device itself: the games keep it in a global, i.e. in their exe's writable data there is a pointer to an
	// object whose vtable's first entries (QueryInterface, AddRef, Release, TestCooperativeLevel) are d3d9.dll code.
	// Reads are exception-guarded; Present/Reset are patched in whatever vtables turn up.
	inline bool ReadPtr(const void* a_at, uintptr_t& a_out)
	{
		__try {
			a_out = *static_cast<const uintptr_t*>(a_at);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	inline int g_adopted = 0;

	// Hooks Present/Reset in the vtables of a device the caller already holds (and its swap chain 0).
	// Returns 1 if a new device vtable was hooked.
	inline int AdoptDevice(IDirect3DDevice9* a_dev)
	{
		void** dvt = *reinterpret_cast<void***>(a_dev);
		int    before = g_nDev;
		Patch(g_devPresent, g_nDev, dvt, kDevPresent, reinterpret_cast<void*>(&DevPresent));
		Patch(g_devReset, g_nReset, dvt, kDevReset, reinterpret_cast<void*>(&DevReset));
		IDirect3DSwapChain9* sc = nullptr;
		if (SUCCEEDED(a_dev->GetSwapChain(0, &sc)) && sc) {
			Patch(g_scPresent, g_nSc, *reinterpret_cast<void***>(sc), kScPresent, reinterpret_cast<void*>(&ScPresent));
			sc->Release();
		}
		return g_nDev > before;
	}

	inline int AdoptGameDevices(HMODULE a_exe)
	{
		auto  base = reinterpret_cast<uintptr_t>(a_exe);
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		auto  d3d = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"d3d9.dll"));
		if (!d3d) return 0;
		auto* dnt = reinterpret_cast<IMAGE_NT_HEADERS*>(d3d + reinterpret_cast<IMAGE_DOS_HEADER*>(d3d)->e_lfanew);
		uintptr_t dlo = d3d, dhi = d3d + dnt->OptionalHeader.SizeOfImage;
		auto*     sec = IMAGE_FIRST_SECTION(nt);
		int       found = 0;
		for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
			if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
			auto* p = reinterpret_cast<uintptr_t*>(base + sec->VirtualAddress);
			size_t n = sec->Misc.VirtualSize / sizeof(uintptr_t);
			for (size_t i = 0; i < n; ++i) {
				uintptr_t obj = 0, vt = 0, f = 0;
				if (!ReadPtr(&p[i], obj) || obj < 0x10000 || (obj & (sizeof(void*) - 1))) continue;
				if (!ReadPtr(reinterpret_cast<void*>(obj), vt) || vt < 0x10000 || (vt & (sizeof(void*) - 1))) continue;
				bool ok = true;
				for (int k = 0; k < 4 && ok; ++k) ok = ReadPtr(reinterpret_cast<void*>(vt + k * sizeof(void*)), f) && f >= dlo && f < dhi;
				uintptr_t present = 0;
				if (!ok || !ReadPtr(reinterpret_cast<void*>(vt + kDevPresent * sizeof(void*)), present) || !present) continue;
				// a device: Reset/Present slots readable, and the object says it is an IDirect3DDevice9
				IDirect3DDevice9* dev = nullptr;
				__try {
					if (FAILED(reinterpret_cast<IUnknown*>(obj)->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void**>(&dev))) || !dev) continue;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					continue;
				}
				found += AdoptDevice(dev);
				dev->Release();
			}
		}
		g_adopted += found;
		return found;
	}

	// Callbacks only: hooks then come from AdoptGameDevices (no throwaway devices are created).
	inline void SetCallbacks(Callback a_onPresent, Callback a_onReset) { g_onPresent = a_onPresent, g_onReset = a_onReset; }

	// Learns d3d9.dll's own Present / swap chain Present / Reset from a throwaway device (the guard's way out of a
	// hook loop) and remembers the callbacks. It patches nothing: here every device has its own copy of the
	// vtable, freed with the device, so hooks in throwaway devices never fire and only filled the table
	// (found by tests/hook_loop_test.cpp, 2026-10-03). The game's own device is hooked by AdoptGameDevices/AdoptDevice.
	// Returns how many of the three functions are known.
	inline int Install(Callback a_onPresent, Callback a_onReset)
	{
		g_onPresent = a_onPresent, g_onReset = a_onReset;
		WNDCLASSEXW wc{ sizeof(wc) };
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"BW2DummyD3D9";
		RegisterClassExW(&wc);
		HWND        wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
		IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
		if (d3d && wnd) {
			const DWORD flags[] = { D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DCREATE_SOFTWARE_VERTEXPROCESSING };
			for (DWORD f : flags) {
				D3DPRESENT_PARAMETERS pp{};
				pp.Windowed = TRUE;
				pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
				pp.hDeviceWindow = wnd;
				IDirect3DDevice9* dev = nullptr;
				if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, f, &pp, &dev)) || !dev) continue;
				void** vt = *reinterpret_cast<void***>(dev);
				if (!g_realDevPresent && InD3d9(vt[kDevPresent])) g_realDevPresent = vt[kDevPresent];
				if (!g_realDevReset && InD3d9(vt[kDevReset])) g_realDevReset = vt[kDevReset];
				IDirect3DSwapChain9* sc = nullptr;
				if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
					void** svt = *reinterpret_cast<void***>(sc);
					if (!g_realScPresent && InD3d9(svt[kScPresent])) g_realScPresent = svt[kScPresent];
					sc->Release();
				}
				dev->Release();
				if (g_realDevPresent && g_realDevReset && g_realScPresent) break;
			}
		}
		if (d3d) d3d->Release();
		if (wnd) DestroyWindow(wnd);
		return (g_realDevPresent != nullptr) + (g_realDevReset != nullptr) + (g_realScPresent != nullptr);
	}
}
