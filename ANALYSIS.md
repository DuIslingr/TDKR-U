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
- `assets/bres.h`, `assets/bdae.cpp`: mesh blocks (self-relative pointers, unprefixed buffers) and a 0xC image stride.
  The check below matches the Python parser's triangle counts on every model.
- Everything above the asset layer (levels, Lua API, HUD, menus, combat) is still N.O.V.A. 3's and expects its data.

`tdkr-check` over the iOS data: `vfs` reads all 6,932 files; `textures` decodes all 1,230; `models` loads all
1,339 (2.3 M triangles).

## Data formats, and how far N.O.V.A. 3's code gets

| Format | Count | Status |
| --- | --- | --- |
| `.gla` custom pak | 37 archives | Same as N.O.V.A. 3, but **names are plain text** (N.O.V.A. 3 XOR32-encrypts them). Android's `CCustomPakReader::scanLocalHeader` has no decode step. Three archives are 16-byte empty paks. `tools/gla.py` detects plain names and empty paks. |
| `.tga` textures | 1230 | Actually **PVR v2** (`PVR!` at +0x2c), mostly PVRTC 4bpp with mips (iOS). N.O.V.A. 3's `textures.py` decodes them. A PC build has to decode PVRTC on load, since desktop GPUs lack it. |
| `.bdae` models | 1339 | BRES like N.O.V.A. 3, with two differences, fixed in `tools/bdae.py`: (1) image records have a 0xC stride (N.O.V.A. 3: 0x10); (2) each mesh is an **out-of-line block**: the geometry points to `{0, n, offset, size, 0}`; the block is `u32 size, u32 size-0x2c, (n-2)` self-relative pointer-slot offsets, then N.O.V.A. 3's `SMesh`, with **self-relative pointers** and **no u32 prefix** on buffers. All 1295 BRES files parse; Batman converts to glTF with textures. |
| `effects/*.bdae` | 44 | ZIPs holding `{big,little}_endian_{quantized,not_quantized}.bdae`, `source.dae`, `dependancies.txt`. |
| `.glsl` | 127 | The game's GLES 2 shaders (`*-v.glsl`/`*-f.glsl`, post effects, BatSonar, CascadedShadow...). N.O.V.A. 3's GLSL to SPIR-V translator is the place to start. |
| `.lvc` levels + `gol.bin` | 12 | Contain Lua 5.1 bytecode (`LuaQ`), the same Lua as N.O.V.A. 3. N.O.V.A. 3's `lvc.py` fails at the first object after `LevelProperties` (object types are game-specific; `gol.bin` defines them). |
| `.irr` scenes | 696 | Irrlicht scene XML. |
| `.nm` / `.edge` / `.index` | 10 each | Navmesh and streaming data per level. |
| `.wav` | 3110 | RIFF, **MS-ADPCM** (format 2), mono 32 kHz. SDL can load these directly. `sounds.xml` + `BATMAN_TDKR.vxa` describe the vox sound events. |
| `.swf` | 104 | **gameswf Flash UI** (SWF v10, uncompressed `FWS`): HUD, menus, shop, minimap. N.O.V.A. 3 has nothing for this (it uses bsprite), so it is the biggest new subsystem. |
| `strings/*.bin` | 198 | Per-level localized strings in 11 languages. |
| `game_config/*.bin` | — | Missions, Rewards, Upgrades, achievements, locations. |
| `options/*.xml` | 23 | Quality presets (GPU/CPU/MEM tiers): useful as the PC "ultra" defaults. |

## Open items found so far

- Batman's cape is missing from the glTF render. Skinning or the controller path is likely different too.
- `.lvc` object types: read `gol.bin` and the Android loader in Ghidra.
- gameswf: decide between reimplementing the parts the UI uses and embedding an SWF player.
- The Batpod, Tumbler and Bat vehicles, plus the open Gotham city streaming (`l_gothamcity`, 350 MB with
  textures) have no N.O.V.A. 3 counterpart.
