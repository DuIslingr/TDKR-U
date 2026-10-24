# The Dark Knight Rises reimplementation

A native desktop reimplementation of Gameloft's *The Dark Knight Rises* (2012), forked from the
N.O.V.A. 3 one (`../N.O.V.A 3`): the same Glitch engine, so the archive, texture, model, shader and
Lua code carries over. Behaviour comes from the Android library (`libKRHP.so`, which ships with its
symbols) in Ghidra. Findings so far are in [ANALYSIS.md](ANALYSIS.md).

**The game's files are not included.** You need your own copy of the iOS build (1.0.4: the `.app`
folder from the IPA).

## Layout

| Path | What |
| --- | --- |
| `engine/` | The engine (C++20, SDL3). Forked from N.O.V.A. 3; so far the archive, texture and model loaders handle TDKR's data |
| `engine/tools/check.cpp` | `tdkr-check`: decoder checks over a data folder |
| `tools/` | Python reference decoders (`gla.py`, `textures.py`, `bdae.py`, `bdae2gltf.py`), symbol matching, Ghidra scripts |
| `work/` | Local only (gitignored): extracted data, Ghidra projects, decompiled code |

## Building (Linux)

With the libraries from the system (Arch: `sdl3 sdl3_ttf zlib lua51 shaderc glm`):

```bash
cmake -S engine -B engine/build -G Ninja
cmake --build engine/build
```

The other platforms build as N.O.V.A. 3 does (`-DTDKR_VENDORED=ON`, `-DTDKR_SHADERCROSS=ON`).

## Checking the data

```bash
engine/build/tdkr-check vfs work/data
engine/build/tdkr-check textures work/data
engine/build/tdkr-check models work/data models.txt
```

`work/data` is the IPA's `Payload/TDKR.app` (its `data/` and `textures/` folders are mounted).
To preview a model: `python3 tools/bdae2gltf.py MODEL.bdae -o out.glb --data work/extracted/data`.
