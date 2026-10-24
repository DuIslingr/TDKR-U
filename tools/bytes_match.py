#!/usr/bin/env python3
"""Pair functions of two builds from the same compiler (iOS 1.0.4 and 1.0.5) by their code:
each function's bytes with the parts that move between builds masked (Thumb BL/BLX targets,
32-bit words that point into the image), hashed; a hash unique on both sides is a match.
Writes "entry<TAB>name<TAB>score" lines (ApplyNames.java) for the named functions.

Usage: bytes_match.py OLD_BINARY old_features.jsonl NEW_BINARY new_features.jsonl out.txt
"""
import hashlib
import json
import struct
import sys
from collections import defaultdict


def segments(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    p, segs = 28, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, p)
        if cmd == 1:
            vm, vs, fo, fs = struct.unpack_from("<IIII", data, p + 24)
            segs.append((vm, vs, fo, fs))
        p += size
    return segs


def reader(path):
    data = open(path, "rb").read()
    segs = segments(data)
    lo = min(s[0] for s in segs if s[1]); hi = max(s[0] + s[1] for s in segs)

    def read(va, n):
        for vm, vs, fo, fs in segs:
            if vm <= va and va + n <= vm + fs:
                return data[fo + va - vm:fo + va - vm + n]
        return None
    return read, lo, hi


def signature(read, lo, hi, entry, size):
    thumb = True   # iOS armv7 is Thumb-2 (Ghidra's entries carry no Thumb bit)
    start = entry & ~1
    code = read(start, size)
    if code is None or size < 8:
        return None
    b = bytearray(code)
    i = 0
    while i + 4 <= len(b):
        h1, h2 = struct.unpack_from("<HH", b, i)
        if thumb and (h1 & 0xF800) == 0xF000 and (h2 & 0xC000) == 0xC000:   # BL / BLX
            b[i:i + 4] = b"\xff\xff\xff\xff"
            i += 4
            continue
        w = struct.unpack_from("<I", b, i)[0]
        if lo <= (w & ~1) < hi:                      # a pointer into the image (literal pool)
            b[i:i + 4] = b"\xee\xee\xee\xee"
            i += 4
            continue
        i += 2
    return hashlib.sha1(bytes(b)).hexdigest() + f":{size}"


def load(path):
    return [json.loads(l) for l in open(path, encoding="utf-8")]


def main():
    old_bin, old_f, new_bin, new_f, out = sys.argv[1:6]
    r_old, lo_o, hi_o = reader(old_bin)
    r_new, lo_n, hi_n = reader(new_bin)
    by_sig = [defaultdict(list), defaultdict(list)]
    for k, (path, r, lo, hi) in enumerate([(old_f, r_old, lo_o, hi_o), (new_f, r_new, lo_n, hi_n)]):
        for f in load(path):
            entry = int(f["entry"], 16) if isinstance(f["entry"], str) else f["entry"]
            s = signature(r, lo, hi, entry, f.get("size", 0))
            if s:
                by_sig[k][s].append((f["entry"], f["name"]))
    pairs = 0
    with open(out, "w") as o:
        for s, olds in by_sig[0].items():
            news = by_sig[1].get(s)
            if len(olds) == 1 and news and len(news) == 1:
                pairs += 1
                name = olds[0][1]
                base = name.split("::")[-1]
                if not base.startswith(("FUN_", "thunk_FUN_", "LAB_", "SUB_", "_GLOBAL__")):
                    o.write(f"{news[0][0]}\t{name}\t100\n")
    print(f"{pairs} functions paired by code")


if __name__ == "__main__":
    main()
