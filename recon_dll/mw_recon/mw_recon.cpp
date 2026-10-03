// Border Warfare 2 - Phase 0b MW2 recon probe (winmm.dll proxy for iw4sp.exe, x64).
//
// Passive: it changes no gameplay. It answers the runtime questions static recon can't:
//   1. Do Pmove's collision queries really go through pmoveHandlers[h].trace (exe+0x3bd440)? How
//      many per second, from which side (cgame prediction / game), with which bounds and masks?
//   2. What does trace_t look like in this x64 build (raw bytes of sampled results)?
//   3. What does MW2 do when it loses focus, and does it keep simulating with focus faked?
// The trace pointers are wrapped by counting/logging functions that call the originals unchanged.
// Fake focus is OFF until `focus on` is written to logs\mw_cmd.txt.
//
// Log: <LogDir>\mw_recon.log. Commands (logs\mw_cmd.txt, one per line):
//   status | focus on|off | sample <n> | con <console command>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../../src/common/fwd_winmm.h"
#include "../../src/common/util.h"

using namespace bw2;

namespace
{
	// ---- this build (Steam, iw4sp.exe x64, TimeDateStamp 0x6aaaa257) ---------------------------------
	constexpr uint32_t  kExeStamp = 0x6aaaa257;
	constexpr uintptr_t kPmoveHandlers = 0x3bd440;  // { trace, debugLine } x 3 handlers, stride 0x10
	constexpr uintptr_t kCgTrace = 0xc8600;         // cgame-side capsule trace (prediction)
	constexpr uintptr_t kGTrace = 0x175280;         // game-side capsule trace
	constexpr uintptr_t kCm = 0x131f800;            // clipMap_t cm (name at +0, isInUse at +8)
	constexpr uintptr_t kCbufAddText = 0x1ee890;    // void Cbuf_AddText(int localClientNum, const char*)
	constexpr uint8_t   kCbufSig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xf9, 0x48, 0x8b, 0xda, 0xb9 };

	using TraceFn = void (*)(void* a_results, const float* a_start, const float* a_end, const float* a_bounds, int a_passEnt, int a_mask);
	using CbufFn = void (*)(int, const char*);

	constexpr int         kSlots = 3;
	TraceFn               g_orig[kSlots] = {};
	std::atomic<uint64_t> g_count[kSlots];
	std::atomic<int>      g_sample[kSlots];
	float                 g_lastStart[kSlots][3];
	float                 g_lastHalf[kSlots][3];
	bool                  g_buildOk = false;
	CbufFn                g_cbuf = nullptr;

	HWND             g_hwnd = nullptr;
	WNDPROC          g_wndProc = nullptr;
	std::atomic_bool g_fakeFocus{ false };
	std::atomic<uint64_t> g_cursorSets{ 0 }, g_cursorGets{ 0 }, g_fgQueries{ 0 };

	decltype(&GetForegroundWindow) g_GetForegroundWindow = nullptr;
	decltype(&GetActiveWindow)     g_GetActiveWindow = nullptr;
	decltype(&SetCursorPos)        g_SetCursorPos = nullptr;
	decltype(&GetCursorPos)        g_GetCursorPos = nullptr;

	bool ReallyFocused() { return g_hwnd && g_GetForegroundWindow && g_GetForegroundWindow() == g_hwnd; }

	// ---- trace wrappers --------------------------------------------------------------------------------
	template <int I>
	void TraceHook(void* a_results, const float* a_start, const float* a_end, const float* a_bounds, int a_passEnt, int a_mask)
	{
		g_orig[I](a_results, a_start, a_end, a_bounds, a_passEnt, a_mask);
		g_count[I].fetch_add(1, std::memory_order_relaxed);
		float b[6] = {};
		SafeRead(b, a_bounds, sizeof(b));
		SafeRead(g_lastStart[I], a_start, 12);
		memcpy(g_lastHalf[I], b + 3, 12);
		if (g_sample[I].load(std::memory_order_relaxed) > 0 && g_sample[I].fetch_sub(1) > 0) {
			float   s[3] = {}, e[3] = {};
			uint8_t r[0x40] = {};
			SafeRead(s, a_start, 12);
			SafeRead(e, a_end, 12);
			SafeRead(r, a_results, sizeof(r));
			char hex[0x40 * 3 + 1];
			for (int i = 0; i < 0x40; ++i) sprintf_s(hex + i * 3, 4, "%02x ", r[i]);
			float frac, n[3];
			memcpy(&frac, r, 4);
			memcpy(n, r + 4, 12);
			Log("trace[%d] pass=%d mask=0x%x start=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f) mid=(%.2f %.2f %.2f) half=(%.2f %.2f %.2f) "
				"-> frac=%.4f n=(%.3f %.3f %.3f)\n    raw: %s",
				I, a_passEnt, a_mask, s[0], s[1], s[2], e[0], e[1], e[2], b[0], b[1], b[2], b[3], b[4], b[5], frac, n[0], n[1], n[2], hex);
		}
	}

	void InstallTraceHooks()
	{
		auto* table = reinterpret_cast<uintptr_t*>(g_exe + kPmoveHandlers);
		const uintptr_t want[kSlots] = { kCgTrace, kGTrace, kGTrace };
		const void*     hooks[kSlots] = { reinterpret_cast<void*>(&TraceHook<0>), reinterpret_cast<void*>(&TraceHook<1>),
            reinterpret_cast<void*>(&TraceHook<2>) };
		for (int i = 0; i < kSlots; ++i) {
			uintptr_t cur = ReadOr<uintptr_t>(reinterpret_cast<uintptr_t>(&table[i * 2]), 0);
			if (cur != g_exe + want[i]) {
				Log("pmoveHandlers[%d].trace = %p, expected exe+%llx: not hooking it", i, reinterpret_cast<void*>(cur),
					static_cast<unsigned long long>(want[i]));
				continue;
			}
			g_orig[i] = reinterpret_cast<TraceFn>(cur);
			SwapPointer(&table[i * 2], reinterpret_cast<uintptr_t>(hooks[i]));
			Log("pmoveHandlers[%d].trace hooked (orig exe+%llx)", i, static_cast<unsigned long long>(want[i]));
		}
	}

	// ---- focus ---------------------------------------------------------------------------------------------
	const char* MsgName(UINT a_msg)
	{
		switch (a_msg) {
		case WM_ACTIVATE: return "WM_ACTIVATE";
		case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
		case WM_SETFOCUS: return "WM_SETFOCUS";
		case WM_KILLFOCUS: return "WM_KILLFOCUS";
		case WM_SIZE: return "WM_SIZE";
		case WM_NCACTIVATE: return "WM_NCACTIVATE";
		default: return nullptr;
		}
	}

	LRESULT CALLBACK WndProcHook(HWND a_hwnd, UINT a_msg, WPARAM a_w, LPARAM a_l)
	{
		if (const char* name = MsgName(a_msg)) {
			bool swallow = false;
			if (g_fakeFocus) {
				swallow = (a_msg == WM_ACTIVATEAPP && !a_w) || (a_msg == WM_ACTIVATE && LOWORD(a_w) == WA_INACTIVE) ||
				          a_msg == WM_KILLFOCUS || (a_msg == WM_NCACTIVATE && !a_w);
			}
			if (a_msg != WM_SIZE || a_w == SIZE_MINIMIZED) Log("msg %s w=0x%llx%s", name, static_cast<unsigned long long>(a_w), swallow ? " (swallowed)" : "");
			if (swallow) return a_msg == WM_NCACTIVATE ? TRUE : 0;
		}
		return CallWindowProcA(g_wndProc, a_hwnd, a_msg, a_w, a_l);
	}

	HWND WINAPI GetForegroundWindowHook()
	{
		g_fgQueries++;
		return g_fakeFocus && g_hwnd ? g_hwnd : g_GetForegroundWindow();
	}

	HWND WINAPI GetActiveWindowHook() { return g_fakeFocus && g_hwnd ? g_hwnd : g_GetActiveWindow(); }

	BOOL WINAPI SetCursorPosHook(int a_x, int a_y)
	{
		g_cursorSets++;
		if (g_fakeFocus && !ReallyFocused()) return TRUE;  // don't drag the cursor away from the game in front
		return g_SetCursorPos(a_x, a_y);
	}

	BOOL WINAPI GetCursorPosHook(LPPOINT a_pt)
	{
		g_cursorGets++;
		if (g_fakeFocus && !ReallyFocused() && g_hwnd) {  // report the window centre: no mouse motion
			RECT r;
			GetClientRect(g_hwnd, &r);
			POINT c{ (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
			ClientToScreen(g_hwnd, &c);
			*a_pt = c;
			return TRUE;
		}
		return g_GetCursorPos(a_pt);
	}

	BOOL CALLBACK FindMain(HWND a_hwnd, LPARAM)
	{
		DWORD pid;
		GetWindowThreadProcessId(a_hwnd, &pid);
		char title[128];
		if (pid == GetCurrentProcessId() && GetWindowTextA(a_hwnd, title, sizeof(title)) && strcmp(title, "Modern Warfare 2") == 0) {
			g_hwnd = a_hwnd;
			return FALSE;
		}
		return TRUE;
	}

	void SetFakeFocus(bool a_on)
	{
		g_fakeFocus = a_on;
		Log("fake focus %s", a_on ? "ON" : "OFF");
		if (a_on && g_hwnd && !ReallyFocused()) {  // the game already saw the deactivation: tell it it's back
			PostMessageA(g_hwnd, WM_ACTIVATEAPP, TRUE, 0);
			PostMessageA(g_hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
			PostMessageA(g_hwnd, WM_SETFOCUS, 0, 0);
		}
	}

	// ---- reporting / commands ------------------------------------------------------------------------------
	void Status()
	{
		std::string map = ReadCStr(ReadOr<uintptr_t>(g_exe + kCm, 0));
		Log("status: map='%s' cm.isInUse=%d focused=%d fake=%d traces cg/g/g2=%llu/%llu/%llu cursorSet/Get=%llu/%llu fgQueries=%llu",
			map.c_str(), ReadOr<int>(g_exe + kCm + 8, -1), ReallyFocused(), g_fakeFocus.load(),
			static_cast<unsigned long long>(g_count[0].load()), static_cast<unsigned long long>(g_count[1].load()),
			static_cast<unsigned long long>(g_count[2].load()), static_cast<unsigned long long>(g_cursorSets.load()),
			static_cast<unsigned long long>(g_cursorGets.load()), static_cast<unsigned long long>(g_fgQueries.load()));
	}

	void RunCommand(const std::string& a_line)
	{
		Log("cmd> %s", a_line.c_str());
		if (a_line == "status") {
			Status();
		} else if (a_line == "focus on" || a_line == "focus off") {
			SetFakeFocus(a_line == "focus on");
		} else if (a_line.rfind("sample ", 0) == 0) {
			int n = atoi(a_line.c_str() + 7);
			for (auto& s : g_sample) s = n;
		} else if (a_line.rfind("con ", 0) == 0) {
			if (!g_cbuf) {
				Log("con: Cbuf_AddText not verified for this build");
				return;
			}
			g_cbuf(0, (a_line.substr(4) + "\n").c_str());
		} else {
			Log("unknown command (status | focus on|off | sample <n> | con <cmd>)");
		}
	}

	DWORD WINAPI Worker(LPVOID)
	{
		uint32_t stamp = ExeTimestamp();
		g_buildOk = stamp == kExeStamp;
		Log("Border Warfare 2 MW2 recon probe. exe base %p, TimeDateStamp 0x%08x (%s)", reinterpret_cast<void*>(g_exe), stamp,
			g_buildOk ? "known build" : "UNKNOWN build: hooks disabled, logging focus only");

		while (!g_hwnd) {
			EnumWindows(FindMain, 0);
			if (!g_hwnd) Sleep(500);
		}
		Log("main window %p", g_hwnd);

		g_GetForegroundWindow = reinterpret_cast<decltype(g_GetForegroundWindow)>(IatHook("USER32.dll", "GetForegroundWindow", &GetForegroundWindowHook));
		g_GetActiveWindow = reinterpret_cast<decltype(g_GetActiveWindow)>(IatHook("USER32.dll", "GetActiveWindow", &GetActiveWindowHook));
		g_SetCursorPos = reinterpret_cast<decltype(g_SetCursorPos)>(IatHook("USER32.dll", "SetCursorPos", &SetCursorPosHook));
		g_GetCursorPos = reinterpret_cast<decltype(g_GetCursorPos)>(IatHook("USER32.dll", "GetCursorPos", &GetCursorPosHook));
		if (!g_GetForegroundWindow) g_GetForegroundWindow = &GetForegroundWindow;
		Log("IAT hooks: GetForegroundWindow=%d GetActiveWindow=%d SetCursorPos=%d GetCursorPos=%d", g_GetForegroundWindow != &GetForegroundWindow,
			g_GetActiveWindow != nullptr, g_SetCursorPos != nullptr, g_GetCursorPos != nullptr);
		if (!g_GetActiveWindow) g_GetActiveWindow = &GetActiveWindow;
		if (!g_SetCursorPos) g_SetCursorPos = &SetCursorPos;
		if (!g_GetCursorPos) g_GetCursorPos = &GetCursorPos;
		g_wndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcHook)));

		if (g_buildOk) {
			if (memcmp(reinterpret_cast<void*>(g_exe + kCbufAddText), kCbufSig, sizeof(kCbufSig)) == 0) {
				g_cbuf = reinterpret_cast<CbufFn>(g_exe + kCbufAddText);
			}
			Log("Cbuf_AddText %s", g_cbuf ? "verified" : "signature mismatch");
			InstallTraceHooks();
			for (auto& s : g_sample) s = 3;
		}

		uint64_t last[kSlots] = {};
		DWORD    nextReport = GetTickCount() + 1000, nextSample = GetTickCount() + 10000;
		for (;;) {
			Sleep(200);
			for (auto& c : TakeCommands(L"mw")) RunCommand(c);
			DWORD now = GetTickCount();
			if (now >= nextReport) {
				nextReport = now + 1000;
				uint64_t c[kSlots];
				for (int i = 0; i < kSlots; ++i) c[i] = g_count[i].load();
				if (c[0] != last[0] || c[1] != last[1] || c[2] != last[2]) {
					Log("traces/s cg=%llu g=%llu g2=%llu  focused=%d  game-trace start=(%.1f %.1f %.1f) half=(%.1f %.1f %.1f)",
						static_cast<unsigned long long>(c[0] - last[0]), static_cast<unsigned long long>(c[1] - last[1]),
						static_cast<unsigned long long>(c[2] - last[2]), ReallyFocused(), g_lastStart[1][0], g_lastStart[1][1], g_lastStart[1][2],
						g_lastHalf[1][0], g_lastHalf[1][1], g_lastHalf[1][2]);
				} else if (g_buildOk) {
					static DWORD quietSince = 0;
					if (!quietSince) quietSince = now;
					if (now - quietSince > 5000) {  // say so once in a while, not every second
						Log("no pmove traces (menu, loading, paused?) focused=%d", ReallyFocused());
						quietSince = now;
					}
				}
				memcpy(last, c, sizeof(last));
			}
			if (now >= nextSample) {  // a few samples every 10 s, so the log has a spread of situations
				nextSample = now + 10000;
				for (auto& s : g_sample)
					if (s <= 0) s = 1;
			}
		}
	}
}

BOOL APIENTRY DllMain(HMODULE a_module, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(a_module);
		InitLog(a_module, L"mw_recon");
		if (HANDLE t = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr)) CloseHandle(t);
	}
	return TRUE;
}
