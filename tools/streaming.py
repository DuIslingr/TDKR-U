#!/usr/bin/env python3
"""Read The Dark Knight Rises' streamed city geometry (CLevelStreaming) and export it to glTF.

Gotham's buildings and roads are not placed as level objects: they are streamed from
l_gothamcity/<Level>[_low_inorm].zip. Read from CLevelStreaming::Load, CStreamedBatchMesh,
CLODStreaming and glitch::io::loadHeadersAndSkipData. All little-endian:

* stream_info.bin: aabbox3d (6 f32, world space, Z up), u32 state-buffer size,
  u32 batch record size.
* batch_info.bin: u32 (= the record size), then one record per batch, each the record
  size long: u16 material index (into <Level>_materials.bdae, a zipped BDAE holding only
  materials), 30 bytes of per-pass vertex attribute mapping, then the glitch vertex
  stream header:
    u8, u32 attribute count, per attribute (u32 offset, u16 semantic, u16 type,
    u16 components, u16 stride); 6 f32 (position offset and scale); per texcoord
    attribute 6 f32 (scale u, scale v, 0, offset u, offset v, 0); u32 vertex count (0);
    24-byte index stream description (u32 primitive type, u32 index type, ...).
  Semantics: 0 position, 1-8 texcoords, 16 normal, 17 colour. Types: 0 s8, 1 u8, 2 s16,
  6 f32 (as in bdae.COMP_FORMATS).
* lod_table.bin: u32 N, N x (u32 offset, u32 size) of the group descriptors in
  lod_data.bin; u32 M, M x u32.
* lod_data.bin: the segments, packed from offset 0, then the N group descriptors:
    u32 id, aabbox3d, u32 LOD count; per LOD u32 segment count; per segment
    u32 segment id, u32 batch, aabbox3d, u32 data offset, u32 (4), u32 vertex bytes,
    u32 index bytes, u16 flags (50 bytes).
  A segment's data: 4 bytes, the batch's interleaved vertices, u16 triangle-list indices.
* lod_selector.bin: per group, 'rang' or 'proj', u32 n, (n-1) f32 distances, (n-1) f32,
  u32 name length, name.
* bih_struct.bin / bih_data.bin: the CRegularGridStreaming cells (not needed to draw).
"""

import argparse
import io
import struct
import sys
import zipfile
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

import bdae  # noqa: E402
import bdae2gltf  # noqa: E402

SEM_POSITION, SEM_NORMAL, SEM_COLOR = 0, 16, 17
GL_FLOAT, GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT = 5126, 5121, 5123, 5125
GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER = 34962, 34963


class StreamError(Exception):
    pass


@dataclass
class Attribute:
    offset: int
    semantic: int
    type: int
    components: int
    stride: int


@dataclass
class Batch:
    material: int
    attributes: list
    texcoord_transforms: dict = field(default_factory=dict)   # semantic -> (scale u, scale v, offset u, offset v)

    @property
    def stride(self):
        return self.attributes[0].stride


@dataclass
class Segment:
    id: int
    batch: int
    bbox: tuple
    data_offset: int
    vertex_bytes: int
    index_bytes: int
    flags: int


@dataclass
class Group:
    id: int
    bbox: tuple
    lods: list          # per LOD, a list of Segment
    selector: tuple = None


class LevelStream:
    def __init__(self, zip_path):
        with zipfile.ZipFile(zip_path) as z:
            files = {Path(n).name: z.read(n) for n in z.namelist()}
        self.name = Path(zip_path).stem
        need = ["stream_info.bin", "batch_info.bin", "lod_table.bin", "lod_data.bin", "lod_selector.bin"]
        missing = [n for n in need if n not in files]
        if missing:
            raise StreamError(f"{zip_path}: missing {missing}")
        self.materials_bdae = next((v for k, v in files.items() if k.endswith("_materials.bdae")), None)
        info = files["stream_info.bin"]
        self.bbox = struct.unpack_from("<6f", info, 0)
        self.state_size, self.record_size = struct.unpack_from("<II", info, 24)
        self.batches = self._batches(files["batch_info.bin"])
        self.data = files["lod_data.bin"]
        self.groups = self._groups(files["lod_table.bin"])
        self._selectors(files["lod_selector.bin"])

    def _batches(self, b):
        rec = self.record_size
        if struct.unpack_from("<I", b, 0)[0] != rec or (len(b) - 4) % rec:
            raise StreamError("batch_info.bin does not match the record size in stream_info.bin")
        out = []
        for i in range((len(b) - 4) // rec):
            p = 4 + i * rec
            material = struct.unpack_from("<H", b, p)[0]
            p += 2 + 30 + 1     # material, attribute mapping (one pass), u8
            n = struct.unpack_from("<I", b, p)[0]
            p += 4
            attrs = [Attribute(*struct.unpack_from("<IHHHH", b, p + 12 * k)) for k in range(n)]
            p += 12 * n + 24    # attributes, position offset and scale
            batch = Batch(material, attrs)
            for a in attrs:
                if 1 <= a.semantic <= 8:
                    su, sv, _, ou, ov, _ = struct.unpack_from("<6f", b, p)
                    batch.texcoord_transforms[a.semantic] = (su, sv, ou, ov)
                    p += 24
            if p + 4 + 24 > 4 + (i + 1) * rec:
                raise StreamError(f"batch {i}: header runs past its record")
            out.append(batch)
        return out

    def _groups(self, t):
        d = self.data
        n = struct.unpack_from("<I", t, 0)[0]
        groups = []
        for i in range(n):
            off, size = struct.unpack_from("<II", t, 4 + 8 * i)
            gid = struct.unpack_from("<I", d, off)[0]
            bbox = struct.unpack_from("<6f", d, off + 4)
            nlod = struct.unpack_from("<I", d, off + 28)[0]
            p, lods = off + 32, []
            for _ in range(nlod):
                nseg = struct.unpack_from("<I", d, p)[0]
                p += 4
                segs = []
                for _ in range(nseg):
                    sid, batch = struct.unpack_from("<II", d, p)
                    sbox = struct.unpack_from("<6f", d, p + 8)
                    data_off, _four, vb, ib = struct.unpack_from("<4I", d, p + 32)
                    flags = struct.unpack_from("<H", d, p + 48)[0]
                    segs.append(Segment(sid, batch, sbox, data_off, vb, ib, flags))
                    p += 50
                lods.append(segs)
            if p != off + size:
                raise StreamError(f"group {i}: descriptor ends at {p:#x}, expected {off + size:#x}")
            groups.append(Group(gid, bbox, lods))
        return groups

    def _selectors(self, s):
        p, i = 0, 0
        while p < len(s) and i < len(self.groups):
            tag, n = s[p:p + 4], struct.unpack_from("<I", s, p + 4)[0]
            p += 8
            dist = struct.unpack_from(f"<{n - 1}f", s, p)
            p += 4 * (n - 1) * 2
            length = struct.unpack_from("<I", s, p)[0]
            p += 4 + length
            self.groups[i].selector = (tag.decode("latin1"), dist)
            i += 1

    def segment_arrays(self, seg):
        """Decoded vertex attributes {semantic: array} and the triangle indices of a segment."""
        batch = self.batches[seg.batch]
        stride = batch.stride
        count = seg.vertex_bytes // stride
        start = seg.data_offset + 4
        raw = np.frombuffer(self.data, np.uint8, count * stride, start).reshape(count, stride)
        out = {}
        for a in batch.attributes:
            fmt, size = bdae.COMP_FORMATS[a.type]
            v = raw[:, a.offset:a.offset + size * a.components].copy().view("<" + fmt).reshape(count, a.components)
            v = v.astype(np.float32)
            if a.semantic in batch.texcoord_transforms:
                su, sv, ou, ov = batch.texcoord_transforms[a.semantic]
                v = v[:, :2] * np.array([su, sv], np.float32) + np.array([ou, ov], np.float32)
            elif a.semantic == SEM_NORMAL and a.type == 0:
                v = v[:, :3] / 127.0
            out[a.semantic] = v
        idx = np.frombuffer(self.data, "<u2", seg.index_bytes // 2, start + seg.vertex_bytes).astype(np.uint32)
        return out, idx


def export(stream, builder, tex_index, warn, lod=0, parent_children=None, region=None):
    """Add the stream's geometry at LOD `lod` (clamped per group) to `builder`: one mesh per
    batch, holding all its segments. `region` (x0, y0, x1, y1) keeps only the groups whose
    box meets that rectangle. Returns the number of segments added."""
    exporter = None
    if stream.materials_bdae is not None:
        data = stream.materials_bdae
        if data[:4] == b"PK\x03\x04":       # zipped like the effects: take the little-endian image
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                data = z.read("little_endian_not_quantized.bdae")
        exporter = bdae2gltf.Exporter(bdae.Bdae(data), tex_index, warn, builder=builder)
    by_batch = defaultdict(list)
    for g in stream.groups:
        if region and (g.bbox[3] < region[0] or g.bbox[0] > region[2] or g.bbox[4] < region[1] or g.bbox[1] > region[3]):
            continue
        if g.lods:
            for seg in g.lods[min(lod, len(g.lods) - 1)]:
                if seg.vertex_bytes:
                    by_batch[seg.batch].append(seg)
    g = builder.json
    added = 0
    for b, segs in sorted(by_batch.items()):
        batch = stream.batches[b]
        parts = defaultdict(list)
        indices, base = [], 0
        for seg in segs:
            arrays, idx = stream.segment_arrays(seg)
            for sem, v in arrays.items():
                parts[sem].append(v)
            indices.append(idx + base)
            base += len(next(iter(arrays.values())))
            added += 1
        arrays = {sem: np.concatenate(v) for sem, v in parts.items()}
        attrs = {"POSITION": builder.accessor(arrays[SEM_POSITION].astype(np.float32), "VEC3", GL_FLOAT,
                                              target=GL_ARRAY_BUFFER, minmax=True)}
        if SEM_NORMAL in arrays:
            n = arrays[SEM_NORMAL][:, :3]
            length = np.linalg.norm(n, axis=1, keepdims=True)
            n = np.divide(n, length, out=np.tile([0, 0, 1], (len(n), 1)).astype(np.float32), where=length > 0)
            attrs["NORMAL"] = builder.accessor(n.astype(np.float32), "VEC3", GL_FLOAT, target=GL_ARRAY_BUFFER)
        for t, sem in enumerate(sorted(s for s in arrays if 1 <= s <= 8)):
            attrs[f"TEXCOORD_{t}"] = builder.accessor(arrays[sem].astype(np.float32), "VEC2", GL_FLOAT,
                                                      target=GL_ARRAY_BUFFER)
        idx = np.concatenate(indices)
        if base <= 0xFFFF:
            ind = builder.accessor(idx.astype(np.uint16), "SCALAR", GL_UNSIGNED_SHORT, target=GL_ELEMENT_ARRAY_BUFFER)
        else:
            ind = builder.accessor(idx, "SCALAR", GL_UNSIGNED_INT, target=GL_ELEMENT_ARRAY_BUFFER)
        prim = {"attributes": attrs, "indices": ind}
        if exporter is not None and batch.material < len(exporter.m.materials):
            prim["material"] = exporter._material(exporter.m.materials[batch.material].id)
        g["meshes"].append({"name": f"{stream.name}_batch{b}", "primitives": [prim]})
        g["nodes"].append({"name": f"{stream.name}_batch{b}", "mesh": len(g["meshes"]) - 1})
        if parent_children is not None:
            parent_children.append(len(g["nodes"]) - 1)
    return added


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("zip", help="streamed level archive, e.g. l_gothamcity/GothamCity.zip")
    ap.add_argument("-o", "--output", help="output .glb (else just print a summary)")
    ap.add_argument("--data", default="work/extracted/data", help="extracted game data (textures)")
    ap.add_argument("--lod", type=int, default=0, help="LOD level to export (0 = most detailed)")
    ap.add_argument("--region", help="X0,Y0,X1,Y1: only the groups that meet this rectangle (world units)")
    args = ap.parse_args(argv)
    s = LevelStream(args.zip)
    segs = [seg for g in s.groups for lod in g.lods for seg in lod if seg.vertex_bytes]
    print(f"{args.zip}: bbox {tuple(round(v, 1) for v in s.bbox)}, {len(s.batches)} batches, "
          f"{len(s.groups)} groups, {len(segs)} segments, LODs per group "
          f"{dict(sorted(__import__('collections').Counter(len(g.lods) for g in s.groups).items()))}")
    if args.output:
        builder = bdae2gltf.GltfBuilder()
        warnings = []
        g = builder.json
        g["nodes"].append({"name": s.name, "rotation": bdae2gltf.ZUP_TO_YUP})
        g["scenes"][0]["nodes"] = [0]
        kids = []
        region = tuple(float(v) for v in args.region.split(",")) if args.region else None
        n = export(s, builder, bdae2gltf.TextureIndex([args.data]), warnings.append, args.lod, kids, region)
        g["nodes"][0]["children"] = kids
        out = builder.glb()
        Path(args.output).write_bytes(out)
        print(f"{args.output}: {n} segments, {len(g['meshes'])} meshes, {len(g['images'])} textures, "
              f"{len(out) / 1e6:.1f} MB")
        for w in sorted(set(warnings))[:20]:
            print(f"  warning: {w}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
