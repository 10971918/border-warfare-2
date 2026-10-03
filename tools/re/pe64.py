"""Read-only x64 PE helper for static recon of the user's own game binaries.

  python pe64.py <exe> str <regex>            strings matching regex, with their RVAs
  python pe64.py <exe> xref <rva|"string">    code sites that reference an RVA (RIP-relative) or a string
  python pe64.py <exe> func <rva>             disassemble the function containing <rva> (.pdata bounds)
  python pe64.py <exe> calls <rva>            direct callers of a function start

Nothing is written next to the game. Caches go to tools/re/cache/ (git-ignored).
"""
import bisect
import os
import pickle
import re
import struct
import sys

import capstone
import numpy as np
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "cache")


class Image:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(size)
        raw = open(path, "rb").read()
        self.secs = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            n = min(s.SizeOfRawData, s.Misc_VirtualSize or s.SizeOfRawData)
            self.mem[s.VirtualAddress:s.VirtualAddress + n] = raw[s.PointerToRawData:s.PointerToRawData + n]
            self.secs.append((name, s.VirtualAddress, max(s.Misc_VirtualSize, s.SizeOfRawData)))
        self.text = next(s for s in self.secs if s[0] == ".text")
        self._funcs = None
        self._refs = None
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True

    def sec(self, name):
        return next(s for s in self.secs if s[0] == name)

    # ---- functions from .pdata (RUNTIME_FUNCTION: begin, end, unwind) ----
    def funcs(self):
        if self._funcs is None:
            _, va, sz = self.sec(".pdata")
            arr = np.frombuffer(bytes(self.mem[va:va + sz]), dtype=np.uint32)
            arr = arr[: len(arr) // 3 * 3].reshape(-1, 3)
            arr = arr[arr[:, 0] != 0]
            # chained unwind entries describe the same function; keep the outermost begin
            self._funcs = sorted(set((int(b), int(e)) for b, e, _ in arr))
            self._starts = [b for b, _ in self._funcs]
        return self._funcs

    def func_of(self, rva):
        """(begin, end) of the .pdata entry covering rva, followed back through chained unwind
        info to the real function start (end stays the fragment's end)."""
        self.funcs()
        i = bisect.bisect_right(self._starts, rva) - 1
        if not (i >= 0 and self._funcs[i][0] <= rva < self._funcs[i][1]):
            return None
        begin, end = self._funcs[i]
        root = self.root_of(begin)
        return (root, end) if root != begin else (begin, end)

    def root_of(self, begin):
        """Follow UNW_FLAG_CHAININFO from a fragment's RUNTIME_FUNCTION to the primary entry."""
        if not hasattr(self, "_unwind"):
            _, va, sz = self.sec(".pdata")
            self._unwind = {}
            for off in range(va, va + sz - 11, 12):
                b, e, u = struct.unpack_from("<III", self.mem, off)
                if b:
                    self._unwind[b] = u
        for _ in range(16):
            u = self._unwind.get(begin)
            if u is None or u & 1:
                return begin
            flags = self.mem[u] >> 3
            if not flags & 4:
                return begin
            n = self.mem[u + 2]
            parent = u + 4 + ((n + 1) & ~1) * 2
            begin = struct.unpack_from("<I", self.mem, parent)[0]
        return begin

    # ---- RIP-relative reference index over .text ----
    def refs(self):
        if self._refs is not None:
            return self._refs
        os.makedirs(CACHE, exist_ok=True)
        key = os.path.join(CACHE, "%s_%d.refs" % (os.path.basename(self.path), os.path.getsize(self.path)))
        if os.path.exists(key):
            self._refs = pickle.load(open(key, "rb"))
            return self._refs
        _, va, sz = self.text
        code = np.frombuffer(bytes(self.mem[va:va + sz]), dtype=np.uint8)
        # Any disp32 whose target lands in the image is a candidate; real instructions are
        # confirmed with capstone on demand. Candidates: modrm with mod=00 rm=101 (RIP-relative),
        # and E8/E9 rel32 calls/jumps.
        refs = {}
        modrm = code[:-4]
        rip = np.nonzero((modrm & 0xC7) == 0x05)[0]
        for kind, idx, ilen_extra in (("rip", rip, 0),):
            pass
        disp = np.frombuffer(bytes(self.mem[va:va + sz]) + b"\0" * 8, dtype=np.uint8)
        def d32(ix):
            return (disp[ix + 1].astype(np.int64) | (disp[ix + 2].astype(np.int64) << 8) |
                    (disp[ix + 3].astype(np.int64) << 16) | (disp[ix + 4].astype(np.int64) << 24)).astype(np.int32).astype(np.int64)
        # rip-relative: target = next_ip + disp; next_ip unknown (imm may follow), assume no imm
        # (+0) and +1/+4 immediates; verify later.
        dd = d32(rip)
        for extra in (0, 1, 4):
            tgt = va + rip + 5 + extra + dd
            ok = (tgt > 0) & (tgt < len(self.mem))
            for site, t in zip((va + rip[ok]).tolist(), tgt[ok].tolist()):
                refs.setdefault(t, []).append((site, "rip%d" % extra))
        calls = np.nonzero((code[:-4] == 0xE8) | (code[:-4] == 0xE9))[0]
        dd = d32(calls)
        tgt = va + calls + 5 + dd
        ok = (tgt >= va) & (tgt < va + sz)
        for site, t, op in zip((va + calls[ok]).tolist(), tgt[ok].tolist(), code[calls[ok]].tolist()):
            refs.setdefault(t, []).append((site, "call" if op == 0xE8 else "jmp"))
        pickle.dump(refs, open(key, "wb"))
        self._refs = refs
        return refs

    def insn_at(self, rva, back=15):
        """Decode the real instruction covering `rva` (a disp32 location), by trying start offsets."""
        for s in range(rva - back, rva + 1):
            for ins in self.md.disasm(bytes(self.mem[s:s + 16]), self.base + s, 1):
                if s + ins.size > rva:
                    return ins
        return None

    def xrefs(self, target):
        """Verified code references to `target` RVA: list of (insn_rva, mnemonic text)."""
        out = []
        for site, kind in self.refs().get(target, []):
            if kind in ("call", "jmp"):
                ins = next(self.md.disasm(bytes(self.mem[site:site + 5]), self.base + site, 1), None)
                if ins and ins.size == 5:
                    out.append((site, "%s %s" % (ins.mnemonic, ins.op_str)))
                continue
            # the modrm byte is at `site`; instruction starts 1-4 bytes earlier
            for start in range(site - 4, site):
                ins = next(self.md.disasm(bytes(self.mem[start:start + 16]), self.base + start, 1), None)
                if not ins:
                    continue
                if "rip" in ins.op_str:
                    for op in ins.operands:
                        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                            if start + ins.size + op.mem.disp == target:
                                out.append((start, "%s %s" % (ins.mnemonic, ins.op_str)))
                    break
        return sorted(set(out))

    def strings(self, pattern):
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern)
        out = []
        for name in (".rdata", ".data"):
            _, va, sz = self.sec(name)
            blob = bytes(self.mem[va:va + sz])
            for m in re.finditer(rb"[\x20-\x7e]{4,}", blob):
                if rx.search(m.group()):
                    # report the start of the C string (previous byte must be 0)
                    out.append((va + m.start(), m.group().decode()))
        return out

    def disasm(self, start, end=None, limit=4000):
        if end is None:
            f = self.func_of(start)
            end = f[1] if f else start + 0x200
        lines = []
        for ins in self.md.disasm(bytes(self.mem[start:end]), self.base + start):
            rva = ins.address - self.base
            note = ""
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    t = rva + ins.size + op.mem.disp
                    note = "  ; ->%x %s" % (t, self.describe(t))
                elif op.type == capstone.x86.X86_OP_IMM and ins.group(capstone.CS_GRP_CALL) or (op.type == capstone.x86.X86_OP_IMM and ins.group(capstone.CS_GRP_JUMP) and ins.mnemonic == "jmp"):
                    t = op.imm - self.base
                    note = "  ; ->%x" % t
            lines.append("%8x  %-8s %s%s" % (rva, ins.mnemonic, ins.op_str, note))
            if len(lines) >= limit:
                break
        return lines

    def describe(self, rva):
        for name, va, sz in self.secs:
            if va <= rva < va + sz:
                if name in (".rdata", ".data"):
                    m = re.match(rb"[\x20-\x7e]{4,}", bytes(self.mem[rva:rva + 120]))
                    if m:
                        return '"%s"' % m.group().decode()[:60]
                return name
        return ""


def parse_rva(s):
    return int(s, 16)


def main():
    img = Image(sys.argv[1])
    cmd = sys.argv[2]
    arg = sys.argv[3] if len(sys.argv) > 3 else ""
    if cmd == "str":
        for rva, s in img.strings(arg)[:200]:
            print("%x  %s" % (rva, s[:120]))
    elif cmd == "xref":
        targets = [parse_rva(arg)] if re.fullmatch(r"[0-9a-fA-F]+", arg) else [r for r, s in img.strings("^" + re.escape(arg) + "$")]
        for t in targets:
            for site, txt in img.xrefs(t):
                f = img.func_of(site)
                print("%x -> %x  in func %s  %s" % (site, t, "%x" % f[0] if f else "?", txt))
    elif cmd == "func":
        r = parse_rva(arg)
        f = img.func_of(r)
        print("func %s" % (("%x-%x" % f) if f else "?"))
        print("\n".join(img.disasm(f[0] if f else r, f[1] if f else None, int(sys.argv[4]) if len(sys.argv) > 4 else 4000)))
    elif cmd == "calls":
        r = parse_rva(arg)
        for site, txt in img.xrefs(r):
            f = img.func_of(site)
            print("%x  in func %s  %s" % (site, "%x" % f[0] if f else "?", txt))


if __name__ == "__main__":
    main()
