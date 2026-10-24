#!/usr/bin/env python3
"""Annotate decompiled C with the values of the literal-pool words it reads.

Ghidra shows ARM literal-pool loads as DAT_xxxxxxxx; tuning constants (speeds, angles,
distances) hide behind them. This appends /* f=<float> i=<int> */ after each DAT_ that
is not part of an address computation (DAT_x + 0x... is PIC addressing and is left alone).

Usage: annotate_dat.py work/bin/libKRHP.so DECOMP.c > DECOMP.annotated.c
"""
import re
import struct
import subprocess
import sys

GHIDRA_BASE = 0x10000


def main():
    so, src = sys.argv[1], sys.argv[2]
    data = open(so, "rb").read()
    loads = [(int(p[2], 16), int(p[1], 16), int(p[4], 16)) for p in
             (line.split() for line in subprocess.run(["readelf", "-lW", so], capture_output=True, text=True).stdout.splitlines())
             if p and p[0] == "LOAD"]

    def word(va):
        for v, o, n in loads:
            if v <= va < v + n:
                return struct.unpack_from("<I", data, va - v + o)[0]
        return None

    def annotate(m):
        w = word(int(m.group(1), 16) - GHIDRA_BASE)
        if w is None:
            return m.group(0)
        f = struct.unpack("<f", struct.pack("<I", w))[0]
        fs = f"{f:.6g}" if abs(f) < 1e7 and (f == 0 or abs(f) > 1e-6) else "-"
        return f"{m.group(0)} /* f={fs} i={w if w < 0x80000000 else w - 0x100000000} */"

    for line in open(src, encoding="utf-8", errors="replace"):
        sys.stdout.write(re.sub(r"\bDAT_([0-9a-f]{8})\b(?!\s*\+)", annotate, line))


if __name__ == "__main__":
    main()
