#!/usr/bin/env python3
"""Assemble a The Dark Knight Rises level (.lvc) into one binary glTF (.glb).

Adapted from the N.O.V.A. 3 project's tools/level2gltf.py (its collision and
navigation layers are left out: TDKR's formats for those are not decoded yet).

Every game object whose mesh component is stored per instance (room geometry,
props, doors, consoles, placed characters) is converted from its .bdae and
placed at the object's CComponentBase transform. Each model is converted once;
placements share its meshes, materials and textures.

Room geometry is authored in world space and placed at the origin, so the
result shows the level as laid out by the designers. Objects whose mesh comes
from the shared object library (most enemies and pickups) use their type's
template from gol.bin; characters appear in their bind pose.
"""

import argparse
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

import bdae  # noqa: E402
import bdae2gltf  # noqa: E402
import lvc  # noqa: E402
import streaming  # noqa: E402


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def euler_deg_to_quat(rx, ry, rz):
    """Glitch/Irrlicht setRotationDegrees order: X, then Y, then Z (q = qz * qy * qx), as (x, y, z, w)."""
    def axis(i, deg):
        h = math.radians(deg) / 2
        q = [0.0, 0.0, 0.0, math.cos(h)]
        q[i] = math.sin(h)
        return tuple(q)
    return _qmul(_qmul(axis(2, rz), axis(1, ry)), axis(0, rx))


# Editor-only stand-ins for invisible objects (trigger boxes, sound emitters).
HELPER_MODELS = ("helpers_box", "staticmeshplaceholder",
                 "editor_", "meshplaceholder", "box_collision")   # the last three: TDKR


def placements(level, gol=None, library=True):
    """(model name, Transform, object name) for every visible placed object.

    The mesh and transform come from the level when stored per instance, otherwise
    (with `library`) from the object type's template in gol.bin or its parents."""
    out = []
    for o in level.objects:
        if o.kind != "object":
            continue
        model, base = o.data.get("model"), o.data.get("base")
        if gol is not None and library and o.type in gol:
            g = gol[o.type]
            if not model:
                t = lvc.resolved_template(g, gol, lvc.COMP_MESH)
                model = t[0] if t else ""
            if base is None:
                t = lvc.resolved_template(g, gol, lvc.COMP_BASE)
                if t:
                    base = lvc.transform(t)
        if model and base is not None and not any(h in model.lower() for h in HELPER_MODELS):
            out.append((model, base, o.name))
    return out


def _transform_node(name, tf, mesh=None):
    node = {"name": name}
    if any(tf.position):
        node["translation"] = list(tf.position)
    if any(tf.rotation):
        node["rotation"] = list(euler_deg_to_quat(*tf.rotation))
    if tuple(tf.scale) != (1.0, 1.0, 1.0):
        node["scale"] = list(tf.scale)
    if mesh is not None:
        node["mesh"] = mesh
    return node


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("level", help=".lvc file")
    ap.add_argument("-o", "--output", required=True, help="output .glb")
    ap.add_argument("--gol", default="work/extracted/data/game_config/gol.bin")
    ap.add_argument("--data", default="work/extracted/data", help="extracted game data (models and textures)")
    ap.add_argument("--skip", default="", help="comma-separated substrings of object or model names to leave out")
    ap.add_argument("--no-library", action="store_true",
                    help="only place objects whose mesh is stored in the level itself")
    ap.add_argument("--no-models", action="store_true", help="leave out the visible models")
    ap.add_argument("--no-stream", action="store_true",
                    help="leave out the streamed city geometry (<Level>.zip, Gotham's buildings and roads)")
    ap.add_argument("--stream-lod", type=int, default=0, help="LOD of the streamed geometry (0 = most detailed)")
    args = ap.parse_args(argv)

    gol = lvc.load_gol(args.gol)
    level = lvc.Level(args.level, gol)
    skip = [s for s in args.skip.split(",") if s]
    places = [] if args.no_models else [p for p in placements(level, gol, not args.no_library) if not any(s.lower() in (p[2] + " " + p[0]).lower() for s in skip)]

    models = {}
    for p in Path(args.data).rglob("*.bdae"):
        models.setdefault(p.name.lower(), p)

    tex_index = bdae2gltf.TextureIndex([args.data])
    builder = bdae2gltf.GltfBuilder()
    texture_map = {}
    warnings = []
    g = builder.json
    g["nodes"].append({"name": Path(args.level).stem, "rotation": bdae2gltf.ZUP_TO_YUP})
    g["scenes"][0]["nodes"] = [0]
    root_children = []

    exporters, missing, failed = {}, Counter(), Counter()
    by_model = defaultdict(list)
    for model, tf, name in places:
        by_model[model.lower()].append((tf, name))
    for model, insts in sorted(by_model.items()):
        path = models.get(model)
        if path is None:
            missing[model] += len(insts)
            continue
        try:
            data = bdae2gltf.load_bdae_bytes(path)
            exp = bdae2gltf.Exporter(bdae.Bdae(data), tex_index, warnings.append,
                                     builder=builder, texture_map=texture_map, skins=False)
        except (bdae.BdaeError, ValueError, KeyError, IndexError) as ex:
            failed[f"{model}: {ex}"] += len(insts)
            continue
        exporters[model] = exp
        for tf, name in insts:
            g["nodes"].append(_transform_node(f"{name}:{model}", tf))
            index = len(g["nodes"]) - 1
            kids = exp.instantiate()
            if kids:
                g["nodes"][index]["children"] = kids
            root_children.append(index)
    extra = []
    if not args.no_stream:
        for zp in Path(args.data).rglob(f"{Path(args.level).stem}.zip"):
            n = streaming.export(streaming.LevelStream(zp), builder, tex_index, warnings.append,
                                 args.stream_lod, root_children)
            extra.append(f"streamed: {n} segments from {zp.name}")
    g["nodes"][0]["children"] = root_children

    out = builder.glb()
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_bytes(out)
    print(f"{args.output}: {len(places)} placements of {len(by_model)} models, "
          f"{len(g['meshes'])} meshes, {len(g['images'])} textures, {len(out) / 1e6:.1f} MB")
    for line in extra:
        print(f"  {line}")
    for m, n in missing.most_common():
        print(f"  missing model: {m} ({n} placements)")
    for m, n in failed.most_common():
        print(f"  failed model: {m} ({n} placements)")
    for w in sorted(set(warnings))[:20]:
        print(f"  warning: {w}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
