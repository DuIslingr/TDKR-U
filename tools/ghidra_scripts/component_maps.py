#!/usr/bin/env python3
"""Map TDKR's component hashes to the functions that read their level and template data.

TDKR identifies components by 32-bit hashes, and many factory cases construct their
class inline (no constructor call to name it), so the maps come from the binary:

* Component classes: every class's vtable slot +0x24 is a GetType() returning its
  hash (CGameObjectManager::CreateObject calls it). Hashes that several classes
  return (a class and its subclass) are settled by the constructor the factory case
  calls, else the shortest name (the base). Hashes no vtable returns are read from
  the factory case itself: its constructor call or its vtable assignment.
* Each class's Load is its vtable slot +8 (what CreateObject calls with the stream).
* Templates (GenerateComponentTemplate): each case stores the template's vptr in
  iVar3 and calls slot +0x10 on it, which is the template's Load.

Inputs: work/bin/libKRHP.so, work/decomp/factory.c (CComponentFactory::CreateComponent),
work/decomp/gentemplate.c (GenerateComponentTemplate). Outputs, for gen_lvc_specs.py:
work/decomp/component_loads.json and work/decomp/templates.json. Run from the repository root.
"""
import json
import re
import struct
import subprocess

SO = "work/bin/libKRHP.so"
GHIDRA_BASE = 0x10000       # Ghidra loads the library at +0x10000

data = open(SO, "rb").read()
loads = [(int(p[2], 16), int(p[1], 16), int(p[4], 16)) for p in
         (line.split() for line in subprocess.run(["readelf", "-lW", SO], capture_output=True, text=True).stdout.splitlines())
         if p and p[0] == "LOAD"]


def word(va):
    for v, o, n in loads:
        if v <= va < v + n and va - v + 4 <= n:
            return struct.unpack_from("<I", data, va - v + o)[0]
    return None


nm = subprocess.run(["nm", "-SC", SO], capture_output=True, text=True).stdout.splitlines()
vtables = []        # (address, size, class)
functions = {}      # address -> demangled name
for line in nm:
    m = re.match(r"([0-9a-f]+) ([0-9a-f]+) [dDvV] vtable for (\w+)$", line)
    if m:
        vtables.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
        continue
    m = re.match(r"([0-9a-f]+) (?:[0-9a-f]+ )?[tTwW] (.+)$", line)
    if m and not m.group(2).startswith("$"):     # skip ARM mapping symbols ($a, $d)
        functions.setdefault(int(m.group(1), 16), m.group(2))
vtable_of = {name: addr for addr, _, name in vtables}


def class_at(va):
    return next((name for a, s, name in vtables if a <= va < a + s), None)


def function_name(addr):
    name = functions.get(addr & ~1, "?")
    return re.sub(r"^non-virtual thunk to ", "", re.sub(r"\(.*", "", name))


def returned_constant(f):
    """The constant an ARM-mode `return K;` function returns, else None."""
    if f is None or f & 1:
        return None
    r0 = None
    for k in range(4):
        w = word(f + 4 * k)
        if w is None:
            return None
        if (w & 0x0FF0F000) == 0x03000000:                      # movw r0, #imm16
            r0 = ((w >> 4) & 0xF000) | (w & 0xFFF)
        elif (w & 0x0FF0F000) == 0x03400000 and r0 is not None:  # movt r0, #imm16
            r0 |= (((w >> 4) & 0xF000) | (w & 0xFFF)) << 16
        elif (w & 0x0FFFF000) == 0x059F0000:                    # ldr r0, [pc, #imm]
            r0 = word(f + 4 * k + 8 + (w & 0xFFF))
        elif (w & 0x0FFFF000) == 0x03A00000:                    # mov r0, #rotated imm8
            rot, v = ((w >> 8) & 0xF) * 2, w & 0xFF
            r0 = ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v
        elif w == 0xE12FFF1E:                                   # bx lr
            return r0
        else:
            return None
    return None


def pic_address(dat, const):
    """Ghidra's `DAT_x + const` (a literal-pool word plus the PC base) as a file address."""
    w = word(int(dat, 16) - GHIDRA_BASE)
    return None if w is None else (w + int(const, 16) - GHIDRA_BASE) & 0xFFFFFFFF


def case_blocks(path, var="param_1"):
    """(hash, lines of the code that runs for it) for each `var == K` / `var != K` test."""
    src = open(path).read().splitlines()

    def after_block(i):     # the code after an `if (x != K) { ... }` block runs for K
        depth, opened = 0, False
        for k in range(i, len(src)):
            depth += src[k].count("{") - src[k].count("}")
            opened = opened or "{" in src[k]
            if opened and depth == 0:
                return k + 1
        return len(src)
    out = []
    for i, line in enumerate(src):
        m = re.search(rf"{var} (==|!=) (0x[0-9a-f]+|\d+)\)", line)
        if m:
            start = after_block(i) if m.group(1) == "!=" else i + 1
            body = []
            for line2 in src[start:start + 40]:
                if re.search(rf"{var} [!=<>]", line2):
                    break
                body.append(line2)
            out.append((int(m.group(2), 0), body))
    return out


# ---------------------------------------------------------------------- components

by_type = {}
for addr, _size, name in vtables:
    t = returned_constant(word(addr + 8 + 0x24))
    if t:
        by_type.setdefault(t, set()).add(name)

components, unresolved = {}, []
for hid, body in case_blocks("work/decomp/factory.c"):
    ctor = next((m.group(1) for line in body for m in [re.search(r"\b(\w+)::\1\s*\(", line)] if m), None)
    candidates = sorted(c for c in by_type.get(hid, ()) if c in vtable_of)
    cls = None
    if len(candidates) == 1:
        cls = candidates[0]
    elif candidates:
        cls = ctor if ctor in candidates else min(candidates, key=len)
    elif ctor:
        cls = ctor
    else:
        # Inline construction: the vptr assigned to the new object, either
        # `iVar4 = DAT_x + K; ... *(int *)this = iVar4 + 8;` or `iVar2 = DAT_x; *(int *)this = iVar2 + K;`.
        pool = {}
        for line in body:
            m = re.search(r"(\w+) = DAT_([0-9a-f]+);", line)
            if m:
                pool[m.group(1)] = m.group(2)
            m = re.search(r"\*\(int \*\)this = (\w+) \+ 0x([0-9a-f]+);", line)
            if m and m.group(1) in pool:
                va = pic_address(pool[m.group(1)], m.group(2))
                cls = va is not None and class_at(va)
                if cls:
                    break
            m = re.search(r"= DAT_([0-9a-f]+) \+ 0x([0-9a-f]+);", line)
            if m:
                va = pic_address(m.group(1), m.group(2))
                cls = va is not None and (class_at(va) or class_at(va + 8))
                if cls:
                    break
    if not cls or cls not in vtable_of:
        unresolved.append(hex(hid))
        continue
    components[f"{hid:#x}"] = {"class": cls, "load": function_name(word(vtable_of[cls] + 8 + 8))}

# ---------------------------------------------------------------------- templates

templates = {}
for hid, body in case_blocks("work/decomp/gentemplate.c"):
    for line in body:
        m = re.search(r"iVar3 = DAT_([0-9a-f]+) \+ 0x([0-9a-f]+);", line)
        if m:
            vptr = pic_address(m.group(1), m.group(2))
            templates[f"{hid:#x}"] = {"class": class_at(vptr) or "?", "load": function_name(word(vptr + 0x10))}
            break
    else:
        unresolved.append(f"template {hid:#x}")

json.dump(components, open("work/decomp/component_loads.json", "w"), indent=1)
json.dump(templates, open("work/decomp/templates.json", "w"), indent=1)
print(f"{len(components)} components, {len(templates)} templates; unresolved: {unresolved}")
