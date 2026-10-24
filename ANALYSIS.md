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
- **Lightmaps**: `LightMapAtlas` from the material, or per object from `CBeastObjectComponent` (atlas region +
  `BakeGroup_*.tga`, bound over the material's `LightMap`). Streamed-city materials name only an inline effect id
  (`#LightMapDC-fx_…`): the loader takes `LightMapDC.bdae` etc. (`StandardDiffuse*`/`SimpleModulate*` → `…All.bdae`).
- **Driver defines** (`shader_compiler.cpp`): `IS_GAME IRRADIANCE LIGHTING BATMAN_SPECULAR CHARACTER_SPECULAR
  USE_FRESNEL_ON_PIXEL`, as the device options switch on. Without `IRRADIANCE` the character shaders read an unwritten
  varying and draw black.
- **Irradiance**: TDKR's `<level>.lvc_<id>.irr` are version 2 (`CIrradianceVolume::fromDataStream`: two flag bytes
  after the set count, low-detail point sets, a truncating grid size). Stadion loads its 10.
- **Visibility at start**: the base component's flags are `c enabled … c visible c always-visible c always-update`
  (`CGameObject::InitComponentBase`); objects not both enabled and visible start hidden. That hides the cutscene rigs
  (`cutscene_*_mesh.bdae`, each with its own Batman, posed where the scene plays).
- **Spawn points**: `CSpawnPointObject::Create` = base, s name, s script, 3 flags (the first: level start), i.
- **Batman** (`game/hero.cpp`): `batman.bdae` + `batman_anims.bdae` (921 clips) at the player, facing where he moves,
  with idle/walk/run/sprint/jump/fall/land clips and the hanging cape (`CAPE`, the gliding cape, hidden). A follow camera
  3 m behind at shoulder height, pulled in front of walls by a collision ray. Movement is N.O.V.A. 3's (ellipsoid on the
  collision, camera-relative WASD + mouse). The level's own Batman placements (script stand-ins) are hidden.
- **Glide** (`Player::gliding`): Batman's states sit in GOL state-set types (`Batman_01_Movement`, `Batman_02_Flying`,
  `Batman_03_Grapnel`, …), which give the clips; the numbers come from `CComponentPlayerComponent`'s template
  (field *k* at offset 4+4k): 17 m/s glide, 25 m/s dive, 2.5 m/s sink, and dive/plunge thresholds of −20°/−40° that
  `CActorBaseComponent::UpdateNavGlide` compares against. Jumping in the air (or falling 0.5 s) opens the cape (the
  spread `CAPE` node); W dives, Shift+W plunges; the camera eases out to 7 m and tilts 20° down.
- **Grapnel** (F): `<level>.edge` holds the grapnel edges (big-endian count, then A, B, up, outward normal per edge;
  Gotham has 6,174). The edge nearest the aim within 50 m and 25° is hooked; a 0.4 s shot, a 25 m/s pull, then the
  `_158_gp_fly_to_idle_stand2` climb (its root starts 2.4 m below the edge).
- **Global illumination** fallback: each zone's `CTemplateGlobalIllumination` record has ambient RGB, a light direction
  and the light's RGB (fields 27–36); outside the irradiance volumes characters fade to a probe made from them.
- `tools/ghidra_scripts/annotate_dat.py` puts literal-pool values (`DAT_…` → float/int) into decompiled code.
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
| `.bdae` models | 1339 | BRES like N.O.V.A. 3, with these differences, handled in `tools/bdae.py` and the engine: (1) image records have a 0xC stride (N.O.V.A. 3: 0x10); (2) the root's visual scenes are at `(0xB0, 0xB4)` (N.O.V.A. 3: `(0xB8, 0xBC)`); (3) meshes **and skins** are **out-of-line blocks**: the geometry (or skin controller) points to `{0, n, offset, size, 0}`; the block is `n` self-relative pointer-slot offsets, then N.O.V.A. 3's `SMesh` (or skin struct), with **self-relative pointers** and **no u32 prefix** on buffers; (4) attributes can be **quantized** (`SMesh +0x28`: per-attribute scale/offset; texcoords are s16). All 1295 BRES files parse; Batman converts to glTF skinned and textured. |
| `effects/*.bdae` | 44 | ZIPs holding `{big,little}_endian_{quantized,not_quantized}.bdae`, `source.dae`, `dependancies.txt`. |
| `.glsl` | 127 | The game's GLES 2 shaders (`*-v.glsl`/`*-f.glsl`, post effects, BatSonar, CascadedShadow...). N.O.V.A. 3's GLSL to SPIR-V translator is the place to start. |
| `.lvc` levels + `gol.bin` | 12 | **All 12 parse** with `tools/lvc.py`, each ending exactly at its -1 marker (Gotham City: 13,643 objects). Lua 5.1 scripts are stored as source text in the string table, plus `LuaQ` bytecode. See "Levels" below. |
| `.irr` | 696 | **Irradiance volumes** (magic `MIhS`), `<level>.lvc_<id>[_low].irr`, one per `irradiance_volume` object. Not Irrlicht scenes. |
| `GothamCity[_Island2][_low_inorm].zip` | 4 | **Gotham's city geometry, streamed** (`CLevelStreaming`). Decoded by `tools/streaming.py` (format in its docstring): 307 batches of 197-byte records (material index + glitch vertex stream header: f32 position, s16 texcoords with per-batch scale/offset, s8 normal), 1,526 LOD groups, 3,706 segments packed in `lod_data.bin`. All four parse. The 42 `*_BEAST` city-block objects name `gc_footprint_XX.bdae` files that don't exist; their meshes come from here. |
| `.nm` / `.edge` / `.index` | 10 each | Per-level navmesh (`<Level>_<id>_navdata.nm`, opened by `CNavMeshNova::Create`), edges and streaming index. |
| `.wav` | 3110 | RIFF, **MS-ADPCM** (format 2), mono 32 kHz. SDL can load these directly. `sounds.xml` + `BATMAN_TDKR.vxa` describe the vox sound events. |
| `.swf` | 104 (51 distinct) | **gameswf Flash UI**: Flash 8–10, uncompressed `FWS`, **ActionScript 2 only**. HUD, menus, shop, minimaps. The engine plays them (see "Flash UI" below). |
| `strings/*.bin` | 198 | Localized strings (MAIN plus one table per level) in 11 languages. Unlike N.O.V.A. 3: **UTF-16LE** text with u32 id lengths and end offsets (`_map.bin`: u16 count, then u32 byte length + UTF-16 id). |
| `game_config/*.bin` | — | Missions, Rewards, Upgrades, achievements, locations. |
| `options/*.xml` | 23 | Quality presets (GPU/CPU/MEM tiers): useful as the PC "ultra" defaults. |

## Flash UI (gameswf)

The UI is 51 distinct movies, in three resolution archives (`gameswf_hires`, `_normalres`, `_lowres`;
`CMenuManager::Init` mounts hires only when the GPU takes 4096² textures). Findings:

- **Bitmaps live in atlases beside the movie**: `<movie>_atlas<id>.tga` (PVR). The SWF keeps an empty
  8×8 `DefineBitsLossless` per atlas id, and the bitmap fills' matrices already address the atlas.
- **Tags in use**: shapes 1–4, `DefineBitsLossless`, `DefineEditText` with `DefineFont3` outlines (one Verdana) or
  device fonts (`Accidental Presidency` → `data/AccidentalPresidency.ttf`), sprites, `PlaceObject2/3`,
  `RemoveObject2`, `FrameLabel`, `ExportAssets`, `DoAction`, `DoInitAction`. No buttons, morphs, sounds or video.
- **ActionScript 2**, a class library compiled from `com.gameloft.components.*`, `src.*`, `scr.*`, `hudAS.*`
  (`Object.registerClass` in init actions; getters/setters through `addProperty`). About 50 opcodes and the usual
  classes (MovieClip, TextField, TextFormat, Array, String, Math, Color, Key, Stage, `flash.geom`).
- **gameswf extensions**: `_global.viewport` (the visible part of the stage in movie units, under "show all"
  scaling), `_global.$version == "gameSWF"`, and 3D clips: `_xrotation`, `_yrotation`, `_z` (the main menu's
  button list is turned by `_global.turnMe`: `_yrotation = -40`, `_rotation = -10`).
- **Game ↔ Flash**: the game calls AS methods (`CharacterHandle::invokeMethod`: `switchStyle`, `setNumber`,
  `enableButton`, `startScreen`…), sets visibility and matrices, and loads movies into clips (`loadMovie`).
  The scripts call 25 natives that `CMenuManager::Init` registers on `_global`: `getString`, `NativeGetString`,
  `NativePushMenu`, `NativeSwitchMenu`, `NativePopMenu`, `NativeStartLevel`, `registerInGameEvent`,
  `registerCarEvent`, `registerInGameEventExtended`, `registerPressSceneButton`, `playSound`, `getScreenConfig`,
  `loadNextStory`, `registerInGameParamValue`, `getGameParamValue`, `registerShopEvent`, `showShopPopup`,
  `canStartNewGame`, `canRestartLevel`, `getConvertedValue`, `getCurrentLanguage`, `registerHUDEvent`,
  `getRealScreenWidth/Height/Ratio`. No `fscommand` and no `ExternalInterface`.
- **HUD** (`CHUDDisplay`, ~200 functions): `BatmanHUD.swf`, root frame `HUD`. `Init` names the button slots and their
  styles (`bAttack` STRIKE/TAKEDOWN/TAKEDOWN2/COUNTERATTACK, `bJump` JUMP/GLIDE/PLUNGE, `bGrapnel`, `bUtility`,
  `gadgetWheel.bGadgets`); `SetButtonStyle` invokes `switchStyle` / `setNumber` / `enableButton` on the slot's
  `button`. The minimap: the level's `<name>_minimap.swf` is loaded into `MinimapPivotInstance.MinimapDummyyInstance`
  and moved so Batman sits at the pivot (`initMap` / `updateMap`): the map's world origin is its `Zero` clip, level
  properties 40 and 41 are pixels per metre and zoom (39 is the movie), the pivot turns with the camera, and
  `CenterMinimap` (the arrow) shows Batman's facing. `SetGrapple` puts `mGraplingHookTarget` over the target.
- **Menus** (`CMenuManager`, one `RenderFX` per movie): `MainMenu.swf`'s `screenManager.startScreen(type)` with
  `main`, `ingame`, `death`; `CHUDDisplay::SwitchMenu` calls it.

The engine's player (`engine/src/swf/`): `movie` (parser), `tess` (even-odd trapezoid fill, like gameswf),
`as`/`as_vm`/`as_builtins` (the AS2 interpreter), `player`/`player_classes` (display list, timelines, events,
MovieClip/TextField/Color/flash.geom, the natives), `library` (resolution and atlases). The renderer side is
`render/swf_draw` (atlases, gradients, SDL_ttf text) and `game/flash_screen`, `game/flash_hud`. Masks clip to
the mask's convex outline. All 51 movies play headless without unhandled opcodes (`tdkr-check swf`).

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

- 46 streamed batches keep the fallback shader (7 effects aren't shipped: `NormalSpecOverbright`, …).
- Batman's other moves (combat, wall contact, hang/edge moves, grapnel to gargoyles/enemies/vehicles, Batpod) and the
  camera's own rules (`CCameraObject`, `DUMMY_CAMERA`). Glide steering and dive input are PC choices, not the game's.
- The fog texture, vertical fog, rain (`MC_WATER`) and reflection shader variants: their textures aren't wired up.
- Streaming in the engine loads all of Gotham at LOD 0 at once; the game streams cells (`bih_*`, `CRegularGridStreaming`) and picks
  LODs by distance (`lod_selector`).
- Stadion's `st_stadium_volumetric` light shafts blend far too bright with the game shader.
- TDKR shaders that skin on the GPU (`SkinWeights`, `SkinIndices`) are not wired to the engine's skinning.
- 114 models referenced in Gotham aren't in the iOS data. 46 are the streamed footprints; the rest (`hc_prop_billboard*`, …) look like cut content.
- Batman's cape is missing from the glTF render. Skinning or the controller path is likely different too.
- Flash UI: the menus render but nothing acts on `NativeSwitchMenu` / `NativeStartLevel` yet (the menu flow,
  `CMenuManager`'s stack, input focus). The HUD shows Batman's on-foot buttons; their states (counter, glide,
  takedown), health, XP, popups, objectives and the joystick for touch are still to be driven from the game.
  Colour transforms' add terms are ignored on bitmaps; non-convex masks clip to their hull; button hit tests use
  bounds.
- The Batpod, Tumbler and Bat vehicles have no N.O.V.A. 3 counterpart.
- Lights: no level uses the light record (0x265E); lighting objects use other components (`CHasLightComponent`, `CSoundAndLight`).
