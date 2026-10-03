// Border Warfare 2 - d3d9hook re-entry guard test (x86, like BL2).
// Recreates the Steam-overlay hook loop on a real device: after "a device reset" another hook is put in front of
// ours that calls ours as its next function, while ours calls that hook as its original. Without the guard the
// two call each other until the stack overflows; with it, Present must return and the loop be counted.
#include <cstdio>

#include "../src/common/d3d9hook.h"

namespace
{
	int   g_presentsSeen = 0;
	void* g_steamNext = nullptr;  // what the fake overlay hook calls next: our hook

	void OnPresent(IDirect3DDevice9*) { ++g_presentsSeen; }

	HRESULT STDMETHODCALLTYPE FakeSteamPresent(IDirect3DDevice9* a_dev, const RECT* a_s, const RECT* a_d, HWND a_w, const RGNDATA* a_r)
	{
		return reinterpret_cast<bw2::d3d9hook::DevPresentFn>(g_steamNext)(a_dev, a_s, a_d, a_w, a_r);
	}

	int Fail(const char* a_what)
	{
		std::printf("hook_loop_test FAILED: %s\n", a_what);
		return 1;
	}
}

int main()
{
	using namespace bw2;
	if (!d3d9hook::Install(&OnPresent, nullptr)) return Fail("no device vtable hooked (no D3D9 device could be created?)");
	if (!d3d9hook::g_realDevPresent) return Fail("d3d9.dll's own Present was not remembered");

	// a device of our own, presenting through a hooked vtable
	HWND wnd = CreateWindowExW(0, L"BW2DummyD3D9", L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp{};
	pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.hDeviceWindow = wnd;
	IDirect3DDevice9* dev = nullptr;
	if (!wnd || !d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)))
		return Fail("test device");
	void** vt = *reinterpret_cast<void***>(dev);
	if (vt[d3d9hook::kDevPresent] != reinterpret_cast<void*>(&d3d9hook::DevPresent)) d3d9hook::AdoptDevice(dev);
	if (vt[d3d9hook::kDevPresent] != reinterpret_cast<void*>(&d3d9hook::DevPresent)) {
		std::printf("vt %p slot %p hook %p, %d device vtables:", vt, vt[d3d9hook::kDevPresent], reinterpret_cast<void*>(&d3d9hook::DevPresent), d3d9hook::g_nDev);
		for (int i = 0; i < d3d9hook::g_nDev; ++i) std::printf(" %p(%p)", d3d9hook::g_devPresent[i].vt, d3d9hook::g_devPresent[i].vt[d3d9hook::kDevPresent]);
		std::printf("\n");
		return Fail("test device not hooked");
	}

	dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
	if (FAILED(dev->Present(nullptr, nullptr, nullptr, nullptr)) || g_presentsSeen != 1) return Fail("normal present");

	// the loop: the overlay re-hooks in front of us (its next = our hook) and we believe it is our original
	g_steamNext = vt[d3d9hook::kDevPresent];
	DWORD old;
	VirtualProtect(&vt[d3d9hook::kDevPresent], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
	vt[d3d9hook::kDevPresent] = reinterpret_cast<void*>(&FakeSteamPresent);
	VirtualProtect(&vt[d3d9hook::kDevPresent], sizeof(void*), old, &old);
	for (int i = 0; i < d3d9hook::g_nDev; ++i)
		if (d3d9hook::g_devPresent[i].vt == vt) d3d9hook::g_devPresent[i].orig = reinterpret_cast<void*>(&FakeSteamPresent);

	HRESULT hr = dev->Present(nullptr, nullptr, nullptr, nullptr);  // without the guard: stack overflow here
	if (FAILED(hr)) return Fail("present through the loop");
	if (d3d9hook::g_loops != 1) return Fail("loop not detected");
	if (g_presentsSeen != 2) return Fail("our callback should run once per present");
	std::printf("hook_loop_test OK (loop broken via d3d9's own Present, %u loop(s))\n", d3d9hook::g_loops.load());
	return 0;
}
