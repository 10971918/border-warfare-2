"""Read-only memory peek into a running game (ReadProcessMemory), for debugging without rebuilding.

  python peek.py iw4sp.exe ptr <rva> [+off ...]     follow pointers from exe+rva, print the final address
  python peek.py iw4sp.exe dvar <rva>               dvar_t* stored at exe+rva: print name and value bytes
  python peek.py iw4sp.exe bytes <rva> <n>          hex dump of exe+rva
  python peek.py iw4sp.exe floats <rva> <n>         n floats at exe+rva
"""
import ctypes
import ctypes.wintypes as W
import struct
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi")
PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x10, 0x400


def find_pid(name):
    import subprocess
    out = subprocess.run(["tasklist", "/fi", "imagename eq " + name, "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == name.lower():
            return int(parts[1])
    raise SystemExit(name + " is not running")


class Proc:
    def __init__(self, name):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, find_pid(name))
        mods = (W.HMODULE * 1024)()
        need = W.DWORD()
        psapi.EnumProcessModulesEx(self.h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
        self.base = mods[0]

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            raise OSError("read %x failed" % addr)
        return buf.raw

    def q(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def cstr(self, addr, n=64):
        return self.read(addr, n).split(b"\0", 1)[0].decode("latin-1")


def main():
    p = Proc(sys.argv[1])
    cmd, rva = sys.argv[2], int(sys.argv[3], 16)
    a = p.base + rva
    if cmd == "ptr":
        for off in sys.argv[4:]:
            a = p.q(a) + int(off, 16)
        print(hex(a))
    elif cmd == "dvar":
        d = p.q(a)
        name = p.cstr(p.q(d))
        print("%s @%x: %s" % (name, d, p.read(d + 0x10, 16).hex(" ")))
    elif cmd == "bytes":
        print(p.read(a, int(sys.argv[4])).hex(" "))
    elif cmd == "floats":
        n = int(sys.argv[4])
        print(struct.unpack("<%df" % n, p.read(a, 4 * n)))


if __name__ == "__main__":
    main()
