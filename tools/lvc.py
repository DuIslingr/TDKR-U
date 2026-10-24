#!/usr/bin/env python3
"""Parse The Dark Knight Rises level files (.lvc) and the game object library (gol.bin).

Adapted from the N.O.V.A. 3 project's tools/lvc.py. Both are big-endian CMemoryStream
"DICT" files, as in N.O.V.A. 3:

* u32 "DICT", u32 offset of the string table, u8 "wide strings" flag; data
  starts at byte 9. The string table is u32 count, then count x (u32 length,
  bytes). In the data, a string is a u32 index into that table.
* .lvc (CLevel::LoadRawLvl / LoadNextObject): u16 'NV', u16 version 3, u32
  object count, then objects: u32 type and that type's fields, no lengths. The
  level-structure types differ from N.O.V.A. 3's (LEVEL_TYPES); any other type
  is a game object whose layout comes from gol.bin.
* gol.bin (CGameObjectManager::Load): u16 'GO', u16 version 3, u32 type count,
  u32 offsets; each entry is i type id, i parent count + parent ids, h component
  count, s name, then (i component hash, h flag) pairs (N.O.V.A. 3: u16 ids and
  the count after the name), then each component's template
  (GenerateComponentTemplate) in listed order.
* A game object in a level (CGameObjectManager::CreateObject) stores, for each
  of its type's own components with flag 0, that component's data: read by the
  class CComponentFactory builds (its Load), or else by the template reader.
  Group objects always store their builtin data. Unlike N.O.V.A. 3 there are no
  components read whatever their flag.

The specs in lvc_specs.py are generated from the decompiled loaders
(tools/ghidra_scripts/gen_lvc_specs.py).
"""

import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lvc_specs import COMPONENT_LOADS, SPECS, TEMPLATE_LOADS  # noqa: E402


class LvcError(Exception):
    pass


class Stream:
    def __init__(self, data):
        if data[:4] != b"DICT":
            raise LvcError("not a DICT stream")
        self.data = data
        table = struct.unpack_from(">I", data, 4)[0]
        self.wide = data[8] != 0
        if self.wide:
            raise LvcError("wide-string streams not supported")
        self.strings = []
        n = struct.unpack_from(">I", data, table)[0]
        p = table + 4
        for _ in range(n):
            length = struct.unpack_from(">I", data, p)[0]
            self.strings.append(data[p + 4:p + 4 + length].decode("latin1"))
            p += 4 + length
        self.end = table
        self.pos = 9

    def _unpack(self, fmt, size):
        if self.pos + size > self.end:
            raise LvcError(f"read past end of data at {self.pos:#x}")
        v = struct.unpack_from(fmt, self.data, self.pos)[0]
        self.pos += size
        return v

    def i(self):
        return self._unpack(">i", 4)

    def f(self):
        return self._unpack(">f", 4)

    def h(self):
        return self._unpack(">h", 2)

    def H(self):
        return self._unpack(">H", 2)

    def c(self):
        return self._unpack(">b", 1)

    def s(self):
        idx = self.i()
        if idx == -1:
            return ""       # GetStringC returns an empty string for missing indices
        if not 0 <= idx < len(self.strings):
            raise LvcError(f"string index {idx} out of range at {self.pos - 4:#x}")
        return self.strings[idx]

    def v3(self):
        return (self.f(), self.f(), self.f())

    def skip(self, n):
        if self.pos + n > self.end:
            raise LvcError("skip past end of data")
        self.pos += n

    def spec(self, spec):
        """Read a flat field list: i f h H c s v (vec3), and [ ... ] for an
        int-count-prefixed repeated group. Returns the values as a list."""
        tokens = spec.replace("[", " [ ").replace("]", " ] ").split()
        out, _ = self._spec(tokens, 0)
        return out

    def _spec(self, tokens, k):
        out = []
        while k < len(tokens):
            t = tokens[k]
            if t == "]":
                return out, k
            if t == "[":
                n = self.i()
                if n < 0 or n > 100000:
                    raise LvcError(f"implausible count {n} at {self.pos - 4:#x}")
                end = k
                items = []
                for _ in range(n):
                    item, end = self._spec(tokens, k + 1)
                    items.append(item)
                if n == 0:
                    depth, end = 0, k + 1
                    while not (tokens[end] == "]" and depth == 0):
                        depth += {"[": 1, "]": -1}.get(tokens[end], 0)
                        end += 1
                out.append(items)
                k = end + 1
                continue
            out.append(getattr(self, t)())
            k += 1
        return out, k



# ---------------------------------------------------------------------- specs

# Hand corrections where the generated spec is wrong (checked against the decompiled code).
SPEC_OVERRIDES = {
    # Same as N.O.V.A. 3: the second list is a list of lists, which the converter misses
    # (it emitted "i [ i i s c ]"); read from the decompiled loader.
    "CComponentCinematic::Load": "i c f c [ i [ i i i i ] ] [ [ i i s c ] ] [ [ i i s ] ] "
                                 "[ s [ i i f f f f f f ] ] [ <CContainerTracksAnimatedObject::Load> ]",
}


def expand_spec(name, seen=()):
    """Spec of loader `name` with nested <X::Load> calls inlined."""
    spec = SPEC_OVERRIDES.get(name, SPECS.get(name))
    if spec is None:
        raise LvcError(f"no spec for {name}")
    if name in seen:
        raise LvcError(f"recursive spec {name}")
    return re.sub(r"<([^>]+)>", lambda m: expand_spec(m.group(1), seen + (name,)), spec)


def spec_of(text):
    """Expand <X::Load> references inside a spec string."""
    return re.sub(r"<([^>]+)>", lambda m: expand_spec(m.group(1)), text)


@dataclass
class Transform:
    id: int
    position: tuple
    rotation: tuple     # degrees
    scale: tuple


def transform(values):
    """CComponentBase::Load values: c i f*9 c c c."""
    return Transform(values[1], tuple(values[2:5]), tuple(values[5:8]), tuple(values[8:11]))


# ---------------------------------------------------------------------- level structure types

BASE = "<CComponentBase::Load>"
MESH = "<CComponentMesh::Load>"     # model name, 4 flags

# CLevel::LoadNextObject: type -> (kind, spec). Navmesh data lives in <Level>_<id>_navdata.nm.
LEVEL_TYPES = {
    0x2657: ("level_properties", "<CTemplateLevelProperties::Load>"),
    0x2667: ("zone", "<CTemplateZone::Load> i"),            # then the zone's byte size
    0x0BBA: ("meta_zone", "<CTemplateMetaZone::Load>"),
    0x265F: ("portal", "<CTemplatePortal::Load>"),          # CZonePortal::Create
    0x2661: ("skybox", "s c f i"),
    0x2662: ("spawn_point", "<CSpawnPointObject::Create>"),
    0x2666: ("waypoint", "<CWayPointObject::Load>"),        # CWayPointObject::Create calls Load
    0x4741: ("waypoint", "<CWayPointObject::Load>"),
    0x798F: ("waypoint_cylinder", "<CWayPointObject::Load> f f f <CComponentBuiltinCylinder::Load>"),
    0x265E: ("light", BASE + " <CComponentLight::Load>"),
    0x267D: ("nav_mesh", BASE),                             # CNavMeshNova::Create
    0x2653: ("occluder", "<CTemplateOccluder::Load>"),
    0x14051: ("long_distance_mesh", BASE + " " + MESH),
    82000: ("reflection_mesh", BASE + " " + MESH),
    99999: ("bake_group", "<CTemplateBakeGroup::Load>"),
    0xDBBB8: ("world_box", "<CComponentBaseWorldBox::Load> <CComponentBuiltinBox::Load>"),
    0x1011: ("irradiance_volume", "<CZone::LoadIrradianceVolume>"),
    0x2669: ("global_illumination", "<CTemplateGlobalIllumination::Load>"),
    0x2664: ("level_int", "i"),
}

# ---------------------------------------------------------------------- game object components

COMP_BASE = 0x0014CA79      # CComponentBase, read inline by CreateObject
COMP_MESH = 0x00152B87      # model name + 4 flags, read inline
COMP_GROUP = 0x2AD2A046     # CGroupObject::ReadBuiltinData, read whatever the flag
GROUP_BUILTIN = "c c [ i ]"


def component_spec(comp):
    """Spec read for a flag-0 component in a level, or '' if nothing is read."""
    if comp in COMPONENT_LOADS:
        return expand_spec(COMPONENT_LOADS[comp][1])
    if comp in TEMPLATE_LOADS:
        return expand_spec(TEMPLATE_LOADS[comp][1])
    return ""   # GenerateComponentTemplate returns nothing: nothing is read


def component_name(comp):
    for table in (COMPONENT_LOADS, TEMPLATE_LOADS):
        if comp in table:
            return table[comp][0]
    return {COMP_BASE: "CComponentBase", COMP_MESH: "CComponentMesh", COMP_GROUP: "CGroupObject"}.get(comp, f"{comp:#x}")


@dataclass
class GolType:
    id: int
    name: str
    parents: list
    components: list    # (component hash, flag)
    templates: dict = field(default_factory=dict)   # component hash -> template values
    template_error: str = ""

    def template_model(self):
        """Mesh name from this type's own mesh template, if any."""
        t = self.templates.get(COMP_MESH)
        return t[0] if t else ""


def load_gol(path, templates=True):
    """Parse gol.bin. With `templates`, also read every component's template data and
    check each entry ends where the next begins."""
    st = Stream(Path(path).read_bytes())
    if st.h() != 0x474F or st.h() != 3:
        raise LvcError("not a version-3 GO library")
    n = st.i()
    offsets = [st.i() for _ in range(n)]
    ends = dict(zip(sorted(offsets), sorted(offsets)[1:] + [st.end]))
    types = {}
    for off in offsets:
        st.pos = off
        tid = st.i()
        parents = [st.i() for _ in range(st.i())]
        ncomp = st.h()
        name = st.s().strip()
        comps = [(st.i(), st.h()) for _ in range(ncomp)]
        gt = GolType(tid, name, parents, comps)
        if templates:
            try:
                for comp, _flag in comps:       # templates follow in listed order
                    spec = expand_spec(TEMPLATE_LOADS[comp][1]) if comp in TEMPLATE_LOADS else ""
                    gt.templates[comp] = st.spec(spec)
                if st.pos != ends[off]:
                    raise LvcError(f"templates end at {st.pos:#x}, next entry starts at {ends[off]:#x}")
            except LvcError as ex:
                gt.templates = {}
                gt.template_error = str(ex)
        types[tid] = gt
    return types


def resolved_template(gtype, gol, comp, seen=()):
    """Template values for `comp`, from the type itself or the nearest parent that has it."""
    if gtype.templates.get(comp) is not None and comp in dict(gtype.components):
        return gtype.templates[comp]
    for parent in gtype.parents:
        if parent in gol and parent not in seen:
            t = resolved_template(gol[parent], gol, comp, seen + (gtype.id,))
            if t is not None:
                return t
    return None


@dataclass
class LevelObject:
    offset: int
    type: int
    kind: str
    name: str = ""
    data: dict = field(default_factory=dict)


def read_game_object(st, gtype):
    data = {}
    for comp, flag in gtype.components:
        if comp == COMP_GROUP:
            data[comp] = st.spec(GROUP_BUILTIN)
        elif flag != 0:
            continue
        elif comp == COMP_BASE:
            data["base"] = transform(st.spec(expand_spec("CComponentBase::Load")))
        elif comp == COMP_MESH:
            data["model"] = st.spec(expand_spec("CComponentMesh::Load"))[0]
        else:
            try:
                spec = component_spec(comp)
            except LvcError as ex:
                raise LvcError(f"component {component_name(comp)} of {gtype.name!r}: {ex}") from None
            if spec:
                try:
                    data[comp] = st.spec(spec)
                except LvcError as ex:
                    raise LvcError(f"component {component_name(comp)} of {gtype.name!r}: {ex}") from None
    return data


class Level:
    def __init__(self, path, gol):
        self.path = Path(path)
        st = self.stream = Stream(self.path.read_bytes())
        if st.h() != 0x4E56:
            raise LvcError("missing 'NV' tag")
        if st.h() != 3:
            raise LvcError("unsupported level version")
        self.count = st.i()     # an editor figure: LoadNextObject reads until -1
        self.objects = []
        while st.pos + 4 <= st.end:
            start = st.pos
            t = st.i()
            if t == -1:
                break
            try:
                self._read_object(st, start, t, gol)
            except LvcError as ex:
                what = gol[t].name if t in gol else LEVEL_TYPES.get(t, ("?",))[0]
                prev = self.objects[-1] if self.objects else None
                after = f"{prev.kind}:{prev.name or hex(prev.type)}@{prev.offset:#x}" if prev else "start"
                raise LvcError(f"in {what!r} (type {t:#x} @ {start:#x}): {ex} [after {after}]") from None
        else:
            raise LvcError("no end marker")
        self.trailing = st.end - st.pos

    def _read_object(self, st, start, t, gol):
        if t in LEVEL_TYPES:
            kind, spec = LEVEL_TYPES[t]
            values = st.spec(spec_of(spec))
            data = {"values": values}
            if spec.startswith(BASE):
                data["base"] = transform(values)
            self.objects.append(LevelObject(start, t, kind, data=data))
        elif t in gol:
            g = gol[t]
            self.objects.append(LevelObject(start, t, "object", g.name, read_game_object(st, g)))
        else:
            raise LvcError(f"unknown object type {t:#x} at {start:#x}")


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("levels", nargs="+")
    ap.add_argument("--gol", default="work/extracted/data/game_config/gol.bin")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    gol = load_gol(args.gol)
    bad = [g for g in gol.values() if g.template_error]
    print(f"gol.bin: {len(gol)} types, {len(bad)} with template errors")
    for g in bad[:5]:
        print(f"    {g.id:#x} {g.name}: {g.template_error}")
    failed = 0
    for path in args.levels:
        try:
            lvl = Level(path, gol)
        except LvcError as ex:
            print(f"FAIL  {path}: {ex}")
            failed += 1
            continue
        status = "OK  " if lvl.trailing == 0 else "WARN"
        print(f"{status}  {path}: {len(lvl.objects)} objects (header says {lvl.count}), "
              f"{lvl.trailing} bytes left")
        if args.verbose:
            for o in lvl.objects:
                print(f"    {o.offset:#08x} {o.type:#07x} {o.kind:18} {o.name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
