// Border Warfare 2: shared helpers for the native DLLs (MW2 guest x64, BL2 host x86).
// Logging, bw2.ini, crash-safe memory access, IAT hooks and the live command file
// (logs\<tag>_cmd.txt, one command per line, consumed and emptied by the running game).
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bw2
{
	inline std::wstring g_logDir = L"A:\\Border Warfare 2\\logs";
	inline FILE*        g_logFile = nullptr;
	inline SRWLOCK      g_logLock = SRWLOCK_INIT;
	inline uintptr_t    g_exe = 0;
	inline HMODULE      g_self = nullptr;

	// ---- bw2.ini next to the DLL: [BW2] Key=value ------------------------------------------------
	inline std::wstring IniPath()
	{
		wchar_t p[MAX_PATH];
		GetModuleFileNameW(g_self, p, MAX_PATH);
		if (wchar_t* s = wcsrchr(p, L'\\')) wcscpy_s(s + 1, MAX_PATH - (s + 1 - p), L"bw2.ini");
		return p;
	}

	inline std::wstring IniString(const wchar_t* a_key, const wchar_t* a_default)
	{
		wchar_t buf[512];
		GetPrivateProfileStringW(L"BW2", a_key, a_default, buf, 512, IniPath().c_str());
		return buf;
	}

	inline int IniInt(const wchar_t* a_key, int a_default)
	{
		return GetPrivateProfileIntW(L"BW2", a_key, a_default, IniPath().c_str());
	}

	// ---- logging -----------------------------------------------------------------------------------
	inline void Log(const char* a_fmt, ...)
	{
		char    buf[4096];
		va_list va;
		va_start(va, a_fmt);
		vsnprintf(buf, sizeof(buf), a_fmt, va);
		va_end(va);
		SYSTEMTIME t;
		GetLocalTime(&t);
		AcquireSRWLockExclusive(&g_logLock);
		if (g_logFile) {
			fprintf(g_logFile, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
			fflush(g_logFile);
		}
		ReleaseSRWLockExclusive(&g_logLock);
	}

	inline void InitLog(HMODULE a_self, const wchar_t* a_tag)
	{
		g_self = a_self;
		g_exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		g_logDir = IniString(L"LogDir", g_logDir.c_str());
		CreateDirectoryW(g_logDir.c_str(), nullptr);
		g_logFile = _wfsopen((g_logDir + L"\\" + a_tag + L".log").c_str(), L"w", _SH_DENYWR);
	}

	// ---- memory --------------------------------------------------------------------------------------
	inline bool SafeRead(void* a_dst, const void* a_src, size_t a_n)
	{
		__try {
			memcpy(a_dst, a_src, a_n);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	template <class T>
	inline T ReadOr(uintptr_t a_addr, T a_default)
	{
		T v;
		return a_addr && SafeRead(&v, reinterpret_cast<const void*>(a_addr), sizeof(T)) ? v : a_default;
	}

	inline std::string ReadCStr(uintptr_t a_addr, size_t a_max = 128)
	{
		char buf[256] = {};
		a_max = a_max < sizeof(buf) - 1 ? a_max : sizeof(buf) - 1;
		for (size_t i = 0; i < a_max; ++i) {
			if (!SafeRead(&buf[i], reinterpret_cast<const void*>(a_addr + i), 1) || !buf[i]) break;
		}
		return buf;
	}

	inline uint32_t ExeTimestamp()
	{
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_exe + dos->e_lfanew);
		return nt->FileHeader.TimeDateStamp;
	}

	// Swaps an aligned pointer-sized value in (possibly read-only) memory; returns the old value.
	inline uintptr_t SwapPointer(uintptr_t* a_at, uintptr_t a_value)
	{
		DWORD old;
		VirtualProtect(a_at, sizeof(uintptr_t), PAGE_READWRITE, &old);
		uintptr_t prev = reinterpret_cast<uintptr_t>(
			InterlockedExchangePointer(reinterpret_cast<void* volatile*>(a_at), reinterpret_cast<void*>(a_value)));
		VirtualProtect(a_at, sizeof(uintptr_t), old, &old);
		return prev;
	}

	// ---- inline hook (x64) --------------------------------------------------------------------------
	// Replaces a_len (14..32) bytes of position-independent prologue, which must equal a_sig, with an
	// absolute jump to a_detour. Returns a trampoline to call as the original, or nullptr when the bytes
	// don't match (another build). Call it while no other thread can be executing the prologue.
#ifdef _WIN64
	inline void* InlineHook(uintptr_t a_target, const uint8_t* a_sig, size_t a_len, void* a_detour)
	{
		if (a_len < 14 || a_len > 32 || memcmp(reinterpret_cast<void*>(a_target), a_sig, a_len) != 0) return nullptr;
		auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!tramp) return nullptr;
		const uint8_t jmpAbs[6] = { 0xFF, 0x25, 0, 0, 0, 0 };  // jmp [rip+0] ; dq target
		uintptr_t     back = a_target + a_len, to = reinterpret_cast<uintptr_t>(a_detour);
		memcpy(tramp, a_sig, a_len);
		memcpy(tramp + a_len, jmpAbs, 6);
		memcpy(tramp + a_len + 6, &back, 8);
		uint8_t patch[32];
		memset(patch, 0x90, sizeof(patch));
		memcpy(patch, jmpAbs, 6);
		memcpy(patch + 6, &to, 8);
		DWORD old;
		VirtualProtect(reinterpret_cast<void*>(a_target), a_len, PAGE_EXECUTE_READWRITE, &old);
		memcpy(reinterpret_cast<void*>(a_target), patch, a_len);
		VirtualProtect(reinterpret_cast<void*>(a_target), a_len, old, &old);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(a_target), a_len);
		return tramp;
	}
#endif

	// ---- IAT hook (main exe imports) ---------------------------------------------------------------
	inline void* IatHook(const char* a_dll, const char* a_func, void* a_detour)
	{
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_exe + dos->e_lfanew);
		auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(g_exe + dir.VirtualAddress); imp->Name; ++imp) {
			if (_stricmp(reinterpret_cast<const char*>(g_exe + imp->Name), a_dll) != 0) continue;
			auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(g_exe + imp->OriginalFirstThunk);
			auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(g_exe + imp->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++addrs) {
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
				auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(g_exe + names->u1.AddressOfData);
				if (strcmp(reinterpret_cast<const char*>(byName->Name), a_func) == 0) {
					return reinterpret_cast<void*>(SwapPointer(reinterpret_cast<uintptr_t*>(&addrs->u1.Function),
						reinterpret_cast<uintptr_t>(a_detour)));
				}
			}
		}
		return nullptr;
	}

	// ---- live command file -----------------------------------------------------------------------------
	// Returns the lines written to <LogDir>\<tag>_cmd.txt since the last call and empties the file.
	inline std::vector<std::string> TakeCommands(const wchar_t* a_tag)
	{
		std::vector<std::string> out;
		std::wstring path = g_logDir + L"\\" + a_tag + L"_cmd.txt";
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE) return out;
		char  buf[8192];
		DWORD n = 0;
		if (ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr) && n) {
			buf[n] = 0;
			SetFilePointer(h, 0, nullptr, FILE_BEGIN);
			SetEndOfFile(h);
			for (char* line = strtok(buf, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
				while (*line == ' ') ++line;
				if (*line && *line != '#') out.emplace_back(line);
			}
		}
		CloseHandle(h);
		return out;
	}
}
