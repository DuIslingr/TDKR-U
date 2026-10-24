# The Dark Knight Rises: first analysis

Gameloft, 2012. Same engine family as N.O.V.A. 3 (Glitch, Gameloft's Irrlicht fork), so the
plan is to follow `../N.O.V.A 3` (a native C++20/SDL3 reimplementation) and reuse what carries over.

## What we have

| File | What it is |
| --- | --- |
| `The Dark Knight Rises v1.0.4 (IOS 4.3+ iPad Optimized).ipa` | Full game: armv7 Mach-O (`TDKR`, decrypted) + all data in `.gla` archives. **The data source.** |
| `TDKR 2022.apk` | Android `com.gameloft.android.GAND.GloftKRLT`, 1.0.0. No data (it came from a dead Gameloft CDN as `datakrhm.obb`), but `lib/armeabi-v7a/libKRHP.so` (16 MB, ARM mode) is **not stripped, with debug info**: 87k symbols, full C++ names. **The code source for Ghidra.** |
| `com.wb.goog.lbbg_2.0.1.17-APK_Award.apk` | LEGO Batman: Beyond Gotham (TT Games, `.fib` data, arm64). A different game and engine, not useful here. |

`work/extracted/ipa`, `work/extracted/apk2022` hold the unpacked IPA/APK (`work/data` links to the `.app`); `work/extracted/data/<archive>/`
holds every `.gla` unpacked (`python3 tools/gla.py extract -o work/extracted/data --per-archive ...`).

## Code (libKRHP.so)

Imports only libc/libm/libGLESv2/liblog: rendering is plain GLES 2, audio goes out through Java
`AudioTrack` (`vox::DriverAndroid`), input arrives through `com.gameloft.glf.GL2JNILib` JNI calls.
Functions by namespace: `glitch` 8.6k, `gameswf` 3.0k, `vox` 1.5k, `glf` 1k, then game classes
(`CActorBaseComponent`, `CNpcAIComponent`, `PlayerComponent`, `CHUDDisplay`, `CLevel`,
`CTrafficControl`, `CBatpodHandleComponent`, patrol units, `CLuaScriptManager`, ...).
Code is shared with N.O.V.A. 3 (`CNavMeshNova` is in this binary). Comparing names in the two
symbolized Android libraries (`work/ghidra/shared_with_nova3.txt`): 16,123 of TDKR's 30,598 named
functions exist in N.O.V.A. 3, and 6,527 of those have the same instruction count. By namespace:
`glitch` 4687/6362, `vox` 900/1143, `glf` 573/747, `spark` (particles) 263/269, `std` 1434/2247.
`gameswf` (0/2491), `glwt`, `onlineServices`, `iap` and nearly all game classes are new.

### Ghidra

| Project | Program | Notes |
| --- | --- | --- |
| `work/ghidra/tdkr.gpr` | `libKRHP.so` | 36,472 functions, 3 unnamed. Ghidra loads it at +0x10000 (`nm` 0x43e540 is Ghidra 0x44e540). Analysis takes ~200 s. |
| `work/ios/tdkr_ios.gpr` | `TDKR` (iOS 1.0.4, armv7, decrypted: `cryptid 0`) | 24,116 functions, 18,817 unnamed before matching. **6,281 named** from libKRHP.so by `tools/match_symbols.py` + `tools/vtable_match.py` (N.O.V.A. 3 got 5,881). The iOS build matches the data we have; Android is 1.0.0. |

Scripts are in `tools/ghidra_scripts` (copied from N.O.V.A. 3). To decompile by name regex:

```bash
/opt/ghidra/support/analyzeHeadless work/ghidra tdkr -process libKRHP.so -noanalysis -readOnly \
    -scriptPath tools/ghidra_scripts -postScript Decomp.java work/decomp/OUT.c "CCustomPakReader::"
```

To rebuild the iOS names: run `ExportFeatures.java` on both programs (giving `work/ghidra/android_features.jsonl`
and `work/ios/ios_features.jsonl`), then `vtable_match.py`, `match_symbols.py`, and `ApplyNames.java` on `tdkr_ios`.

## Engine (forked from N.O.V.A. 3)

`engine/` is N.O.V.A. 3's engine with `nova3`/`nova` renamed to `tdkr`. TDKR-specific changes so far:

- `core/vfs`: plain-text pak names, empty paks, archives in `data/` and `textures/`, and loose files under `data/`
  mounted as entries (`Entry::Loose`).
- `assets/texture`: `.tga` names hold PVR v2; the few real Targa files (intro logos) decode through `decode_tga`.
- `assets/bres.h`, `assets/bdae.cpp`: mesh blocks (self-relative pointers, unprefixed buffers), a 0xC image stride, and
  **quantized attributes**: `SMesh +0x28/+0x2c` holds, per attribute, a pointer to `(scale xyz, offset xyz)` or null. TDKR
  stores texcoords as s16 (scale ~1.5e-5) that way. The check below matches the Python parser's triangle counts on every model.
- `assets/level.cpp` (+ generated `level_specs.inc`, `tools/gen_engine_specs.py`): TDKR's level types, hashed components,
  GOL layout and flag-0 rule, read until -1. All 12 levels load, with the same object counts as `tools/lvc.py`.
- `assets/stream.cpp` + `Scene::load_level`: a level's `<Level>.zip` streamed city becomes one model (LOD 0, world space).
- Shaders: TDKR attribute names (`Position`, `Coord0`, `Coord1`, `Color`), a `LOD_BIAS -1.0` driver define (several shaders
  sample with it but never define it), and `Coord<n>_scaleoffset` / `LightMapAtlas` set to identity (the shaders dequantize
  texcoords themselves; the loader already has).
- Fog from `CComponentBaseGlobalIllum`: level-properties fields 44–45 (start, end) and 46–49 (RGBA).
- Everything else above the asset layer (Lua API, HUD, menus, combat, zones) is still N.O.V.A. 3's and keyed to its
  small component ids, which find nothing in TDKR's data.

`tdkr-check` over the iOS data: `vfs` reads all 6,932 files; `textures` decodes all 1,230; `models` loads all
1,339 (2.3 M triangles); `levels` loads all 12; `shaders` compiles 376 of 406 effect passes (the rest reference
missing shader files, `shadow2DEXT`, or one effect whose shader name parses as garbage).

In the game (`tdkr --level Stadion.lvc --no-scripts --screenshot OUT.bmp`), Stadion draws at night with the game's own
shaders, and Gotham (`--level GothamCity.lvc`) draws its streamed city and props. Side-by-sides with the gameplay
playlist (Android, 13 parts, `work/youtube`, 360p) are in `work/compare/`. The city is brighter than the footage: no
lightmaps yet, so no baked night lighting.

## Data formats, and how far N.O.V.A. 3's code gets

| Format | Count | Status |
| --- | --- | --- |
| `.gla` custom pak | 37 archives | Same as N.O.V.A. 3, but **names are plain text** (N.O.V.A. 3 XOR32-encrypts them). Android's `CCustomPakReader::scanLocalHeader` has no decode step. Three archives are 16-byte empty paks. `tools/gla.py` detects plain names and empty paks. |
| `.tga` textures | 1230 | Actually **PVR v2** (`PVR!` at +0x2c), mostly PVRTC 4bpp with mips (iOS). N.O.V.A. 3's `textures.py` decodes them. A PC build has to decode PVRTC on load, since desktop GPUs lack it. |
| `.bdae` models | 1339 | BRES like N.O.V.A. 3, with two differences, fixed in `tools/bdae.py`: (1) image records have a 0xC stride (N.O.V.A. 3: 0x10); (2) each mesh is an **out-of-line block**: the geometry points to `{0, n, offset, size, 0}`; the block is `u32 size, u32 size-0x2c, (n-2)` self-relative pointer-slot offsets, then N.O.V.A. 3's `SMesh`, with **self-relative pointers** and **no u32 prefix** on buffers. All 1295 BRES files parse; Batman converts to glTF with textures. |
| `effects/*.bdae` | 44 | ZIPs holding `{big,little}_endian_{quantized,not_quantized}.bdae`, `source.dae`, `dependancies.txt`. |
| `.glsl` | 127 | The game's GLES 2 shaders (`*-v.glsl`/`*-f.glsl`, post effects, BatSonar, CascadedShadow...). N.O.V.A. 3's GLSL to SPIR-V translator is the place to start. |
| `.lvc` levels + `gol.bin` | 12 | **All 12 parse** with `tools/lvc.py`, each ending exactly at its -1 marker (Gotham City: 13,643 objects). Lua 5.1 scripts are stored as source text in the string table, plus `LuaQ` bytecode. See "Levels" below. |
| `.irr` | 696 | **Irradiance volumes** (magic `MIhS`), `<level>.lvc_<id>[_low].irr`, one per `irradiance_volume` object. Not Irrlicht scenes. |
| `GothamCity[_Island2][_low_inorm].zip` | 4 | **Gotham's city geometry, streamed** (`CLevelStreaming`). Decoded by `tools/streaming.py` (format in its docstring): 307 batches of 197-byte records (material index + glitch vertex stream header: f32 position, s16 texcoords with per-batch scale/offset, s8 normal), 1,526 LOD groups, 3,706 segments packed in `lod_data.bin`. All four parse. The 42 `*_BEAST` city-block objects name `gc_footprint_XX.bdae` files that don't exist; their meshes come from here. |
| `.nm` / `.edge` / `.index` | 10 each | Per-level navmesh (`<Level>_<id>_navdata.nm`, opened by `CNavMeshNova::Create`), edges and streaming index. |
| `.wav` | 3110 | RIFF, **MS-ADPCM** (format 2), mono 32 kHz. SDL can load these directly. `sounds.xml` + `BATMAN_TDKR.vxa` describe the vox sound events. |
| `.swf` | 104 | **gameswf Flash UI** (SWF v10, uncompressed `FWS`): HUD, menus, shop, minimap. N.O.V.A. 3 has nothing for this (it uses bsprite), so it is the biggest new subsystem. |
| `strings/*.bin` | 198 | Per-level localized strings in 11 languages. |
| `game_config/*.bin` | — | Missions, Rewards, Upgrades, achievements, locations. |
| `options/*.xml` | 23 | Quality presets (GPU/CPU/MEM tiers): useful as the PC "ultra" defaults. |

## Levels

`CLevel::LoadNextObject` reads `u32 type` and that type's fields until -1. The header count is an
editor figure (Gotham says 14,917, but stores 13,643). TDKR's level-structure types (N.O.V.A. 3's are 0x3A–0x1011):

| Type | Object | Reader |
| --- | --- | --- |
| 0x2657 | level properties | `CTemplateLevelProperties::Load` (level scripts, `l_*.gla`, menu/minimap `.swf`, fog, GI) |
| 0x2667 | zone | `CTemplateZone::Load`, then an int: the zone's byte size (inactive mission zones are skipped by it) |
| 0x0BBA | meta zone | `CTemplateMetaZone::Load` |
| 0x265F | portal | `CZonePortal::Create` → `CTemplatePortal::Load` |
| 0x2661 | skybox | `s c f i` (model, flag, float, id) |
| 0x2662 | spawn point | `CSpawnPointObject::Create` |
| 0x2666, 0x4741 | waypoint | `CWayPointObject::Create`, a virtual call to `Load` |
| 0x798F | waypoint with cylinder | waypoint, `f f f`, `CComponentBuiltinCylinder::Load` |
| 0x265E | light | `CComponentBase::Load` + `CComponentLight::Load` |
| 0x267D | nav mesh | `CComponentBase::Load` only; the mesh is in `_navdata.nm` |
| 0x2653 | occluder | `CTemplateOccluder::Load` |
| 0x14051 / 82000 | long-distance / reflection mesh | Base + Mesh |
| 99999 | bake group (Beast lightmap atlas) | `CTemplateBakeGroup::Load` |
| 0xDBBB8, 0x1011, 0x2669, 0x2664 | world box, irradiance volume, global illumination, an int | their own readers |
| other | game object (type from `gol.bin`) | `CGameObjectManager::CreateObject` |

Game objects differ from N.O.V.A. 3 in three ways:

- Component ids are **32-bit hashes** (`CComponentBase` 0x14CA79, mesh 0x152B87, group 0x2AD2A046).
- A GOL entry is `i id, i nparents, i×n, h ncomp, s name, ncomp × (i hash, h flag)`, then the templates in order.
- A component's data is in the level **only if its flag is 0**, read by the class's `Load` (vtable +8) or by its template reader.
  The one exception is the group builtin data (`c c [i]`), which is always read. There are no N.O.V.A. 3-style "always read" components.

"BEAST" components (`CBeastObjectComponent`) hold Illuminate Labs Beast lightmap atlas offsets, not geometry.

**Spec pipeline** (all in `tools/ghidra_scripts`, run from the repo root):
1. `Decomp.java`: every `::Load` → `work/decomp/loaders.c`; the level-object `Create`s → `levelobjs.c`;
   `CComponentFactory::CreateComponent` → `factory.c`; `GenerateComponentTemplate` → `gentemplate.c`.
2. `skel2spec.py loaders.c levelobjs.c > specs.json`. Fixed for TDKR: `while( true )` list loops, inlined
   `ReadChar` (`*(int *)(param_1 + 0xc) = iVar + k`), calls wrapped over two lines, and streams passed as `in_r1`.
3. `component_maps.py`: hash → class through each vtable's `GetType` (slot +0x24), its `Load` (slot +8), and the
   template `Load` (slot +0x10). Covers 92 components and 157 templates.
4. `gen_lvc_specs.py` → `tools/lvc_specs.py`. One hand override is in `lvc.SPEC_OVERRIDES` (`CComponentCinematic`, as in N.O.V.A. 3).

`gol.bin`: 653 types, and 643 read their templates exactly. The other 10 are library entries for level-structure types
(LevelProperties, Portal, Occluder…), each 4–16 bytes short. N.O.V.A. 3 has the same gap.

`tools/level2gltf.py` places every object's model (from the level, or its type's template) and adds the level's streamed city:
Stadion gives 70 placements of 16 models, and the stadium bowl renders in place. Gotham gives 2,320 prop placements plus 3,041 streamed
segments (buildings, roads, bridges) in the same world space.

## Open items found so far

- **Lightmaps**: Beast atlases (`BakeGroup_*.tga`) per object (`CBeastObjectComponent`: atlas scale/offset + texture) and per
  streamed batch (the 30-byte attribute map and materials don't name them yet); without them the scenes lack baked night lighting.
- Streaming in the engine loads all of Gotham at LOD 0 at once; the game streams cells (`bih_*`, `CRegularGridStreaming`) and picks
  LODs by distance (`lod_selector`).
- Stadion's `st_stadium_volumetric` light shafts blend far too bright with the game shader.
- TDKR shaders that skin on the GPU (`SkinWeights`, `SkinIndices`) are not wired to the engine's skinning.
- 114 models referenced in Gotham aren't in the iOS data. 46 are the streamed footprints; the rest (`hc_prop_billboard*`, …) look like cut content.
- Batman's cape is missing from the glTF render. Skinning or the controller path is likely different too.
- gameswf: decide between reimplementing the parts the UI uses and embedding an SWF player.
- The Batpod, Tumbler and Bat vehicles have no N.O.V.A. 3 counterpart.
- Lights: no level uses the light record (0x265E); lighting objects use other components (`CHasLightComponent`, `CSoundAndLight`).
