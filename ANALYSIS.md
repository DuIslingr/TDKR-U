# The Dark Knight Rises: first analysis

Gameloft, 2012. Same engine family as N.O.V.A. 3 (Glitch, Gameloft's Irrlicht fork), so the
plan is to follow `../N.O.V.A 3` (a native C++20/SDL3 reimplementation) and reuse what carries over.

## What we have

| File | What it is |
| --- | --- |
| `DarkKnightRises.apk` | A 2026 community rebuild of Gameloft's Android **Amazon 1.1.6** build (see below). Its `assets/gamedata.zip` is Gameloft's Android data. **The default data source**: it matches the footage and has the controller prompts. |
| `The Dark Knight Rises v1.0.4 (IOS 4.3+ iPad Optimized).ipa` | Full game: armv7 Mach-O (`TDKR`, decrypted) + all data in `.gla` archives. The first data source; still supported (`work/data_ios`). |
| `The_Dark_Knight_Rises___1.0.5_ios_6.0(1).ipa` | iOS 1.0.5, a cracked repack (see below). Data nearly identical to 1.0.4. |
| `TDKR 2022.apk` | Android `com.gameloft.android.GAND.GloftKRLT`, 1.0.0. No data (it came from a dead Gameloft CDN as `datakrhm.obb`), but `lib/armeabi-v7a/libKRHP.so` (16 MB, ARM mode) is **not stripped, with debug info**: 87k symbols, full C++ names. **The code source for Ghidra.** |
| `com.wb.goog.lbbg_2.0.1.17-APK_Award.apk` | LEGO Batman: Beyond Gotham (TT Games, `.fib` data, arm64). A different game and engine, not useful here. |

`work/data` is the Android data (`gamedata.zip`'s `data/` and `textures/`), and `work/extracted/data/<archive>/` holds
its `.gla` archives unpacked (`python3 tools/gla.py extract -o work/extracted/data --per-archive ...`). The iOS
equivalents are `work/data_ios` (a link to `work/extracted/ipa/Payload/TDKR.app`) and `work/extracted/data_ios`;
`work/extracted/android` keeps `gamedata.zip` and the APK's code. Findings above that compare against
"the data" were made on iOS 1.0.4 unless they say otherwise; the engine and tools pass the same checks on both.

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
| `work/ios/tdkr_ios.gpr` | `TDKR` (iOS 1.0.4, armv7, decrypted: `cryptid 0`) | 24,116 functions, 18,817 unnamed before matching. **6,281 named** from libKRHP.so by `tools/match_symbols.py` + `tools/vtable_match.py` (N.O.V.A. 3 got 5,881). |
| `work/ios105/tdkr_ios105.gpr` | `TDKR` (iOS 1.0.5) | 23,931 functions. **5,819 named** from 1.0.4: `match_symbols.py` (strings, calls) plus `tools/bytes_match.py` (same compiler: code with branch targets and image pointers masked); the two agree on all 1,289 functions both name. Full decompile: `work/decomp_ios105/all.c`. |
| `work/amazon/tdkr_amazon.gpr` | `libKRASarm32.so` (Android/Amazon 1.1.6, `lib_Android.so`, no DWARF) | 25,918 functions; 3,924 named by its exports, **7,431 more** matched from libKRHP.so. Built differently (fewer, larger functions), so code pairing does not apply. Full decompile: `work/decomp_amazon/all.c`. |
| `work/krasrt/krasrt.gpr` | `libKRAS.so` (the redmemories arm64 runtime) | Unstripped. Its game hooks: `work/krasrt/game_hooks.c`. |

Scripts are in `tools/ghidra_scripts` (copied from N.O.V.A. 3). To decompile by name regex:

```bash
/opt/ghidra/support/analyzeHeadless work/ghidra tdkr -process libKRHP.so -noanalysis -readOnly \
    -scriptPath tools/ghidra_scripts -postScript Decomp.java work/decomp/OUT.c "CCustomPakReader::"
```

For iOS 1.0.5: `ExportFeatures.java` on the named 1.0.4 program and on 1.0.5, then `match_symbols.py` and
`bytes_match.py` (1.0.4 binary + features, 1.0.5 binary + features), their union through `ApplyNames.java`.

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

### Android: the Amazon build in `DarkKnightRises.apk`

Not a Gameloft release: a 2026 community rebuild (signed `REVIVED`, `com.redmemories.darkknightrises`)
that runs Gameloft's **Amazon build 1.1.6** (`com.gameloft.android.AMAZ.GloftKRAS`) on arm64 Android:

- `libKRASarm32.so`: Gameloft's original 32-bit game library (soname `lib_Android.so`; engine strings
  `1.5.21` as in our libKRHP.so; 3,603 exported C++ names, no DWARF).
- `libKRASaot.so` (71 MB): that library translated ahead of time to arm64 (exports only `aot_*`).
- `libKRAS.so`: their runtime (Dynarmic ARM recompiler, oaknut, JNI shims for Gameloft's Java classes).
  Its game hooks only translate Gameloft's data and save paths into the app's folders, hook
  `registerInGameEvent` (Amazon `0x3e98ac`) to log menu pages, and log shop purchases and Wayne coins.
  No gameplay changes. The Java side (`classes.dex`, 41 KB) is their own shell.
- `assets/gamedata.zip`: Gameloft's Android data (1.9 GB unpacked), the same layout as iOS plus
  device-specific Flash archives (`gameswf_i9000/i9100/i9220/i9250/i9300`). The engine reads it:
  `--data work/apk_new/and`.

Against iOS 1.0.4, the Android data:
- **Textures** are ETC1 (PVR format `0x36`) instead of PVRTC4, a few RGB565/RGB888; Flash atlases are
  `rgb.pvr` + `alpha.pvr` zips with the alpha at half size (plus a `SPLIT` marker).
- **Is what the footage shows**: its `MainMenu.swf` puts Tech Shop on a white banner and says
  "Collection"; it adds confirm popups (`screenConfirmExOneButton/TwoButtons`, `InGameConfirmPopUp`), shop
  items (skill-point packs, a super health restorer), a minigame skip button, and `MainMenu2.swf`.
- **Adds controller support**: ~140 strings for the Xperia Play and PowerA/MOGA pads (tutorials, loading hints:
  "Press the right trigger to use your grapnel gun").
- Rebalances the shop (Batarangs in packs of 20, not 5), changes its own shader set (GLES variants), the
  shop level and Police Station slightly; Gotham's streaming zips are rebaked.

### iOS 1.0.5 (`The_Dark_Knight_Rises___1.0.5_ios_6.0(1).ipa`)

A later build (iOS 6+, armv7, `cryptid` 0). **This IPA is a cracked repack**: it injects
`libhzm.dylib` / `Haima.hz` (a third-party store) and links MobileSubstrate IAP-bypass hooks
(`iap.dylib`, `IAPFreeService.dylib`). Only its data is used here; its binary is never run. The
uncompressed content is the same size as 1.0.4 (1.54 GB; the IPA is just stored, not deflated). Against 1.0.4:

- **Textures**: identical. **Scripts, levels, strings, sounds, SWFs**: identical (every `.gla` CRC differs
  only because the archives were rebuilt).
- **Models**: re-exported with the same content; the only bytes that change are the export timestamps in
  inline effect ids (`#…-fx1392992010_…` → `-fx1405329388_…`, Feb → Jul 2014) and a few padding bytes.
- **Gotham**: 127 new `*_low.bdae` models (footprints, islands, trees, billboards, bridges, railway) replace
  the dropped low-end streaming zips (`GothamCity[_Island2]_low_inorm.zip`); the full-detail streaming zips are
  rebaked with the same layout (307 batches, 1,526 groups, 3,706 segments). They are low-end stand-ins for the
  streamed city: loading them as the missing Gotham models (114 → 31 missing) doubles the buildings, so the
  engine does not.
- **gameswf_lowres** atlases are 512² (were 256²); `gameswf_normalres` menu backgrounds are re-encoded.
- **Code**: platform work only. It adds the Facebook SDK (login, posting) and the advertising identifier
  (`ASIdentifierManager`), and drops the MAC-address and keychain code (Apple's iOS 7 rules).

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
- **Clip events**: components get their parameters (a Text's `style`, `align`…) from the placed instance's
  `onClipEvent(construct)`, which runs before the class constructor. A script setting a transform property to the
  value it already has (the Text component's `_xscale = 100`) does not take the clip off its timeline's moves.
- **Menus** (`CMenuManager`, one `RenderFX` per movie): `MainMenu.swf`'s `screenManager.startScreen(type)` with
  `main`, `ingame`, `death`; `CHUDDisplay::SwitchMenu` calls it, `CHUDDisplay::EnterIGM` loads `MainMenu.swf`
  into the HUD's `mHackArea` for the pause (`ingame`) and death screens. The title is `MenuPressKey.swf`; the
  main menu's backdrop is the `l_menu` level through `mm_city_camera.bdae`'s Camera001 (its animation segment is
  kept in the file: state 1, size 0, data at the runtime-pointer offset).
- **Events**: the scripts report presses with `registerInGameEvent(NAME, arg)`; the game compares about 60
  names (`NEW_GAME` → `GS_BaseMenu::StartNewGame`, mission `STR_FPS_LEVEL_01_TUTORIAL`; `CONTINUE_GAME`;
  `MISSION_ACTIVATED` → `StartChapter(index)`; `CHAPTER_LIST_INIT` → `InitChapterSelection`: `init`,
  `addToList(chapter, title)`, `init`, `addMissionToChapter(box, n, enabled, id, …)`; `IGMMODE` (Continue),
  `BUTTON_RESTART_CHECKPOINT`, `BUTTON_RESTART_LEVEL`, `BACK_TO_MAIN_MENU`, `EXIT_GAME`, `SHOP_PARENT`…).
- **Missions**: level properties field 8 lists each level's missions: start Lua, name, menu background, chapter,
  number, chapter picture, selection/index/description strings, first-mission flag. Chapters 1–6 are the story,
  7 the free roam.
- **HUD buttons** restyle through the slot's `button`: `switchStyle(STYLE)` (`hudAS.hudID` maps a style to its
  icon and action: STRIKE, TAKEDOWN, COUNTERATTACK, GRAPNEL, JUMP/GLIDE/PLUNGE, gadgets…),
  `setNumber(n)`, `enableButton()` / `disableButton()` (60% alpha).
- The footage's Android `MainMenu.swf` puts Tech Shop on a white banner; iOS's (1.0.4 and 1.0.5) makes it
  yellow text.

The engine's player (`engine/src/swf/`): `movie` (parser), `tess` (even-odd trapezoid fill, like gameswf),
`as`/`as_vm`/`as_builtins` (the AS2 interpreter), `player`/`player_classes` (display list, timelines, events,
MovieClip/TextField/Color/flash.geom, the natives), `library` (resolution and atlases). The renderer side is
`render/swf_draw` (atlases, gradients, SDL_ttf text) and `game/flash_screen`, `game/flash_hud`. Masks clip to
the mask's convex outline. All 51 movies play headless without unhandled opcodes (`tdkr-check swf`).

## Level scripts

Missions start as the menus start them: the mission's level, then its start Lua (`--mission N`; the tutorial is
`Enable(900161) TeleportPlayerToZone(900163)`). The level's string table is level properties field 10
(`020_gothamcity.xml` → `strings/020_GothamCity_*`; N.O.V.A. 3 used field 24).

**The script API.** `RegisterScriptFunctions()` registers **723 natives** one `CLuaScriptManager::RegisterCFunction`
call at a time; `tools/gen_script_api.py` follows its ARM code (each name and function is a PC-relative literal) to
list them into `engine/src/script/game_api_names.inc`. All are registered, so scripts never call nil: those not
implemented log their first use and count their calls (`--trace-scripts` prints the count at the end of a headless run).
TDKR's own natives are in `engine/src/game/tdkr_api.cpp`; the constants (`STATE_TYPE_*` bits, `FACE_*`, `WEAPON_*`,
`IGNORE_CONTROLS_*`, `DEATH_*`, ...) come from `CLuaScriptManager::RegisterScriptGlobalVariables`,
`CHUDDisplay::m_faces` and `CAIController::Script_Init`. The scripts' id for Batman is 0.

- **Components** are hashed in TDKR: trigger `0x37F7A8AE` (the same fields as N.O.V.A. 3's; target -1 is the player),
  box `0x729EC572` and cylinder `0x749126C3` shapes, interaction `0x34747EBE` (radius, ?, script with
  `Obj<id>_OnScriptEvent`, button frame), quest `0x55A6393B`, main story `0x5863BD59`, state set `0x5441A117`.
- **Triggers** run `OnEnter`, `OnExit`, `WhileIn`, `WhileOut`, or, if defined, `Obj<id>_Always` every update instead
  of the tests (each start counts against the run count). Spawn points run `Obj<id>_OnSpawn` when the player is
  teleported there (`PlayerComponent::TeleportInSpawnpoint`). Threads see their object as `CurrentActor`.
- **Quests** (`CQuestManager`, `CQuestComponent::Update`): statuses 0 disabled, 1 enabled, 2 initialising, 3 running,
  4/5/6 failed, 7 completed, 9/10/11 canceled, 12 accepted. Enabled runs `Obj<quest>_InitQuest` and moves on to 2,
  where `_Enabled` runs whenever it is not running; Accepted runs `_Accepted` then 3, where `_Running` runs whenever
  it is not running (the scripts poll there); Completed runs `_Completed` once. `CLuaScriptManager::StartFunction`
  never starts a function its object is already running, which makes these per-update starts safe.
- **The main story** is the `MainStory` gol type's list (quest, level string, `Templ18253_<n>_InitQuest`, `_Briefing`,
  `_SkipMission`, reminder seconds, `_OnScriptEvent`). `EnableMainStory` starts the current entry; completing its
  quest advances to the next. `NextMission(zone)` makes a mission zone current (`CZonesManager::NextMission`; its
  `Obj<zone>_Init` runs).
- **Actors**: a state set lists states as (name, `STATE_TYPE_*` mask, ?, loop, ..., clips at [27]); NPC types inherit
  theirs from several parents (`Male_Civilian_Movement`, `Decor_Social`, `Couple_Anims`, ...), all on
  `batman_anims.bdae`. `ActorSetState` plays one of the state's clips; `ActorIsInStateType` tests the mask.
  `NPCGoToWaypoint` follows a waypoint chain, running each `Obj<wp>_OnReach` (cars and the helicopter too).
- **Cameras**: `PlayCameraAnim(object, clip, loop[, wait])` makes the object's animated camera the view
  (`CCameraMgr::SetMaxCamera`) until `EndCinematic` / `CameraSwitchToPlayer`.

- **Fights** (`engine/src/game/brawl.cpp`, a simplification of CMeleeComponent / CNpcAIComponent): the state sets
  carry everything they need. Batman's light strikes are `Batman_Attack_LS*`, his counters
  `Combat_Attack_0x_<Jab|Cross|Kick>Counter` against the enemy's `Enemy_Hurt_0x_*Counter`, the enemy attacks
  `Enemy_Attack_01_Jab` .., their hurts `Enemy_Hurt_01_Jab` .., death `Enemy_Cross_Death`; the silent takedown is
  `Silent_Takedown_Choke_Attacker` / `_Victim` (mask 0x800, STATE_TYPE_SPECIAL, which the tutorial tests), the leap
  from above `Gargoyle_Jump_Fall_On_Enemy_Start` .. `Gargoyle_Jump_Land_On_Enemy` / `Silent_Takedown_From_Gargoyle`.
  `NPCAttackTarget(enemy, 0)` (or stance "Aggressive") sets an enemy on Batman. `IsCounterAttackControlEnabled` is
  `CHUDDisplay::IsUIButtonEnabled(5, 7)`: the scripts allow the counter *and* an attack can be countered now (the
  tutorial freezes time on it). `HasPlayerAttacked` is a strike, not a takedown. `ActorGrapnelEnable` is
  `SetTargetable`; `GrapplingLockTarget(wp)` sends the next grapnel shot to a waypoint.
- **Snipers** (`NPCSetSniper(actor, watched, stand, sweep)`, CSniperComponent's sweep / aim / attack, simplified):
  `Sniper_Aim_Sweep` around the watched point, `Sniper_Aim_Idle` once Batman is in a few degrees of the aim and in
  sight, `Sniper_Shoot` after a moment, `Sniper_Reload`. **The grapnel pull**: at an enemy (aimed at, or locked with
  `GrapplingLockTarget(enemy, true)`) it plays `Grapnel_Enemy_Shoot` and `Enemy_Grapnel_Loopable_Pull_Start / _Fly /
  _Land`, then `Enemy_Grapnel_Pulled_Down_Floor_Idle` and `_Get_Up`, and the enemy fights.
- **Slow motion** (`SetSlowMotion(factor, in, hold, out)`, CSlowMotion::Set / Update): ramp to the factor over `in` ms,
  hold, ramp back over `out` (all 0: it stays); `ResetSlowMotion(ms)` ramps back; `ResetObjectSlowMotion(0, ms)` is
  Batman's, which is the game's. The tutorial freezes time (factor 0) while it teaches the counter.
- **The win screen** (`SetWinScreen(title, xp, wayne tech, ...)`): the HUD's `WinScreen` frame (`mWinScreen`: the
  completed quest's title in `headerText.textB`, `t_xp`, `t_wayneTech`, `missionTime`), `m_mission_complete`, and the
  button's `CONTINUE_FROM_WIN`. `CHUDDisplay::SetWinScreen` also calls `setScores` / `setTitle`, which this movie
  does not define. Checkpoints show `CheckpointMessageInstance` (`FPS_GAME_SAVED`).
- **Objective markers** (`ObjectiveAdd(object, kind)`): one copy of the HUD's `ObjectiveBtn` (`hudAS.mNPCIcon`) per
  object, on the frame of its kind (`kill`, `objective`, `follow`, `talk`, `sideObjective`, ...; `<kind>_arrow` at the
  screen's edge when off screen) with the distance in `DistanceToObjectiveText`.

**The tutorial** (mission 0) now runs from its scripts: trigger 900161 enables quest 222; its `InitQuest` hides
Batman for the stand-in on the gargoyle (900281) and enables 900207, whose `Always` spawns the street's NPCs and
traffic and plays camera 900283 under Alfred's lines (subtitles in the `mCinematicHUD` bar); `EndCinematic`, the
limo drives off along its waypoints, `SetQuestAccepted(222)`, and `_Running` steps through the prompts: look around,
the checkpoints, the silent takedown on sniper 900510, the leap onto 900592, the grapnel to the next roof, the two
guards (strike, counter), the grapnel pull on sniper 900426, the vantage point, the glide through the rings
(900519 → 900526 → 900607 → 900608), `QTE_AutoGrapnel(900586)` onto the last roof, trigger 900618's
`SetQuestCompleted(222)`, the win screen ("The Adventure Begins", 50 XP, 25 WayneTech), and the main story's next
quest (834). A headless run plays all of it in about 220 game seconds with `--did look,move --fight-test` and a few
`--exec-at` teleports (and `ActorLookTo(0, ring)` to steer the glide) in place of the player. Still missing: the
dialog portraits, traffic and the monorail, the snipers' laser sights.

**Mission 1** (chapter 1.2, quest 834 "From the bar to the Stock Exchange"; `--mission 1`) runs from its scripts to its
end: the intro (TDKR's cinematic object: script camera keys), the binoculars over Joe's Bar, the police raid and the
explosions, the street fight, the batarang lesson, the thug Selina kicks down, the conversation in the alley, the cop,
Alfred's call; then `Completed` and the main story's quest 836. What it needed:

- **Cinematic objects** (`CCinematicComponent` `0x17AC851F`): `[0]` length ms, `[2]` speed, `[6]` script tracks
  (`Obj<id>_<track>_<n>_OnScriptEvent`), `[7]` camera tracks (`'Linear'`: `[ms, end, position, target]` relative to the
  object, interpolated between keys; `CCinematicComponent::UpdateCamera`), `[8]` per-object clips
  (`[clip, start, speed, loop, length]`). `StartCinematicObject` returns `2, length` without waiting;
  `PlayAnimation(object, clip, wait, loop)` waits only when asked.
- **The binoculars** (`SetHudState('Binoculars')`, `StartGadgetZoom`, `GadgetZoomTarget(target, k, ms, ms, ?, dx, dy, dz)`,
  `StopGadgetZoom(ms)`; `PlayerComponent::*GadgetModeCinematic`): the camera starts at Batman's eye, looks at each
  target and moves `k` of the way towards it over `ms`, seen through the HUD's `Binoculars` frame (`BinocularsHudMC`,
  stretched across the screen) in night vision (a green-tinted grey).
- **Conversation shots** (`CameraFollowObject(camera, node, dx, dy, dz, ?, target, node, dx, dy, dz, ?, ms)`): the
  camera at an object 1.6 m up plus the offset (the scripts' eye-level dummies carry -1.6), looking at another plus its
  offset, cut (`ms` 0) or panned. `IsActorLookingAt(a, b, angle)` passes `angle` to `cosf` as it is (radians).
- `Enable()` / `Disable()` without an object act on the script's own; `KillActor(actor, state)` plays that death
  state; enemies are those whose NPC AI component (`0x0113F791`) has `[1]` set (behaviour `[0]`: 0 ranged, 2 melee,
  1 sniper, 6 police, 7 pedestrian, ...). The batarang: `SetWeaponAmmo("Batarang_01", n)`, `SetBatarangControlEnable`,
  `Batarang_Throw`. `GetQuestsWithStatus(name)` returns the quest objects in that status. TDKR's effect component
  (`0x2CA13C06`): effect, ?, node, autostart. Batman stays under physics in cinematics (he is seen in them).

**Quest 836 and the Stock Exchange** (chapter 1.3, quest 171 in `StockExchange.lvc`): quest 836's Gotham part ends
in `RequireLoadLevel("STR_FPS_LEVEL_STOCKEXCHANGE", 701998)` (the quests and the main story carry over); spawn trigger
702019 enables quest 171. Its scripts play the police briefing through the binoculars, the fights in the hall, the
search for the hostages (doors kicked open, actors freed with the use button), two consoles hacked to open rooms, a
fake hostage who fights, Lucius's call, Bane's speech and the statue Batman pulls down on Bane's men; then
`SetWinScreen('Waynetech', 100, 25)` and `RequireLoadLevel("STR_FPS_LEVEL_GOTHAM", 1900032)`, where quest 836
completes and the main story moves on to quest 1000. `--auto-play` (with `--fight-test`) plays all of it headless.
What it needed:

- **The hacking game** (`StartPipeGame(win, lose, grid, size, skip price, ms, start row, end row)`;
  `CHUDDisplay::StartPipeMinigame`): the HUD goes to its `Hacking` frame, `minigame.swf` loads into `mHackArea` and
  `miniGame.init(grid, rows, columns, start row, end row, ms)` starts it (the grid string is one digit per circle,
  row by row). The movie reports `registerInGameEvent("GAME_PIPE", "WIN" | "LOSE")` (its skip button
  `SKIP_MINIGAME_BUTTON`); `CHUDDisplay::WinHack` / `LoseHack` run the win or lose function, and `ResetAfterHack`
  empties `mHackArea`, puts the `HUD` frame back and turns the controls on. Its music is `m_hack_minigame`.
- **The QTE button and the pull** (`EnableMiniGameObject(object, on)`, `QTE_EnableControl(object, ms, win, lose)`;
  `PlayerComponent::SetQTEParams`, `CHUDDisplay::AddButtonToScene`): a QTE button (`mQTEButtonInstance`) over the
  object for `ms`; pressed in time, the win function runs and Batman's grapnel pulls the minigame object
  (`PullableObjectComponent` `0x4891830C`: idle, struggle, fall and back clips, then the struggle's ms): its struggle
  clip, then its fall clip and its `OnScriptEvent`. Missed, the lose function (which asks again).
- **Interaction targets** (`SetInteractionParams(object, radius, target, frame)`; `CInteractionComponent::OnInteract`):
  using an object with a target enables the target (the hostages enable trigger 702971, which reads
  `GetCurrentInteractionObjectID()`) instead of running its own `OnScriptEvent`. A trigger enabled again starts its
  run count over (`CTriggerComponent::Enable`).
- **Enabled is not visible**: `CComponentBase`'s first flag enables an object, the visible flag shows it; the usable
  planes (`flippedplane.bdae` interactive objects) are enabled and invisible.
- `NPCGoToWaypointWithNavmesh(actor, waypoint)` (two arguments; at a run), `GetEnemiesInRange(object, radius[, height])`
  (nearest first, at most 18), `GetFrameTime()` (ms), `NPCSetTarget`, `SpawnObjectNearActor`, `EnableTalkMode` (its
  radio click). Post effects (`PostProcessingEffectAdd/Remove`: RadialBlur, ShockWave, HeatHaze, ColorCorrection with a
  16x16x16 LUT from `commons_tex`), Gotham's traffic and monorail, the side missions and achievements are accepted
  and not done yet. TDKR's `EnableControls` leaves the glide alone (`SetGlideControlEnable` is
  `CBaseControlScheme::EnableGlideControl`, a design flag of its own).

**Quest 1000, the Batpod tail** (chapter 1.3, Gotham after the Stock Exchange; the main story's
`Templ18253_3_InitQuest` enables it): Bane escapes in two vans (cinematics 702932, 702933), Lucius sends the Batpod
(1900022, used: `EnterBatPod`), and the vans drive their waypoint paths (speeding up at 702838 / 702899 and
702842 / 702903) while `Obj1900068_Running` warns under 35 m and fails the quest under 25 m. At waypoint 702858 the
van has arrived: trigger 1900274 opens; Batman gets off (`ExitBatPod`), the station cinematic (702966) plays, and
`RequireLoadLevel("STR_FPS_LEVEL_UNDERGROUND", 700002)` starts quest 838 (quest 170 in `Underground.lvc`). The
footage's player tails the van on foot across the rooftops instead; both work.

- **The Batpod** (`CBatpodHandleComponent`, component `0x0D1839C4`: `[0]` mass 100, `[3]` 4 m/s backwards, `[6]`
  the engine's force 1000, `[7]` the brakes' 500; the forward top speed is the upgrades' item 16, not in
  `Upgrades.bin`, so 40 m/s here): force / mass forward, brakes then a slow reverse, steering on a radius that widens
  with speed, kept on the ground, sliding along walls (`engine/src/game/batpod.cpp`). Batman sits on it with
  `batman_anims.bdae`'s `_751_batpod_idle`, `_752_batpod_ride`, `_753/_754_batpod_lean*_c` (made for the bike's
  origin), the camera further back, and the HUD shows `mCarInterfaceInstance` instead of Batman's buttons
  (`CHUDDisplay::SetBatpodInterface`). `IsPlayerOnTheBatOrBatpod()` keeps the combat music on while riding
  (`StartCombatMusic`: the music gives way to `env_music_action_low`; `StopCombatMusic(handle)`).
- **A failed mission restarts**: `MissionFailedAutoReload` is `CLevel::MissionFailed` with its reload, and
  `CLevel::RespawnInit` ends in `CQuestManager::InitStoryQuest(true)`: the current story quest is entered again, its
  `InitQuest` enabling it afresh. Here a respawn keeps the quests and the story, and a story quest that failed (or is
  off) starts over.
- **QTE navigation** (`QTE_StartNavigation` / `QTE_EndNavigation` / `QTE_IsInNavigation`,
  `PlayerComponent::*QTENavigation`): a chase mode with the interface off, in which Batman runs from waypoint to
  waypoint (`CWayPointObject::GenerateRandomNext`, `CMotionComponent::StartMoveToPosition`) and grapnels between
  targets the scripts set (`QTE_GrapplingSetTarget(target, waypoint, ...)`). Quest 836 starts it behind the
  helicopter; here the mode and the grapnel targets are kept, the runs between waypoints are not done yet.
- String tables can list an id twice (`020_GothamCity`'s `STR_C_STOCKEXCHANGE_TRANZITION8`, the second empty): the
  first text counts.

**The Underground** (chapter 1.4, main story quest 838; quest 170 in `Underground.lvc`, from spawn 700002): Selina
escapes onto the tracks (cinematics), the fights on the platforms, three junction boxes hacked (4x4 grids) to move
the train, Selina caught behind bars and the conversation, the snipers' room whose three lights the batarang breaks,
the lift (another hack), the machine-gun room's QTE, the tunnels and Lucius's call, the crowbar ambush, then Bane: a
scripted fight whose beats a timer drives (`underground_KillCounter` += `GetFrameTime()`), quest 170 completed, the win
screen, the `underground_end` storyboard and `RequireLoadLevel("STR_FPS_LEVEL_THEPIT")`. What it needed:

- `IsDead(id)` is true for what is not an actor with health (`CZonesManager::FindActor`): nothing there, or an object
  without NPC AI (the scripts test a mistyped id, 40017, and the electric turret 300360).
- **Breakable things** (`CDestroyableComponent` `0x068D1134`: `[1]` death clip, `[4]` sound, `[5]` effect, `[6]`
  disabled on death, `[8]` script): the batarang breaks them (the lights have 10 health); the clip and sound play and
  its `OnScriptEvent` runs (`OnDieEvent`). The death effect is not played yet.
- **Storyboards** (`StartIntroStory(story, callback, ...)`: `CHUDDisplay::StartStory` over `StoryboardPlayer.swf`):
  `StoryBoard.xml`'s frames (a texture, its ms, its move: `pan_*`, `zoom_in` / `zoom_out`, an optional caption)
  shown full screen; the pictures are the top 1024x768 of their 1024x1024 textures. A key or click skips; then the
  callback runs (`engine/src/game/storyboard.cpp`). Captions are not drawn yet.
- `ActorIsDetected(actor)` (an enemy whose awareness has it as target), `NPCGoToActorWithNavmesh(actor, target)` (to
  where the target is; to Batman: a fight), `NPCGoToObject`, `NPCReturnToSavedPosition`, `NPCSetEnemyFlag`,
  `SetInvincible` (hits leave health alone), `LinkObject(object, parent, node, dx, dy, dz, ...)` /
  `UnlinkObject` (carried at the parent's node plus the offset), `NoiseEvent(object, ?, metres)` (enemies in earshot
  come), `QTE_DisableControl`, `HideObjectNode`, `DisableControlEvent("MoveFast")` (no running). Sound switches,
  muted state sounds, the awareness cone, the forced target, laser targeting and the electric turrets are accepted.
- Batarangs are inventory: mission 1 gives 10 (`SetWeaponAmmo`) and pickups refill them; a level started on its own
  has none.

**The Pit** (`ThePit.lvc`, loaded by the Underground's storyboard callback; quest 603): from its start spawn (1500003)
Bane's speech in the cinematic, the `the_pit_intro` storyboard ("day 3, 95, 179"), then Bruce Wayne
(`brucepit.bdae`, animated with `batman_anims.bdae`) follows a prisoner, takes the two guards out with the steam valves,
fights the inmates, works the rock puzzle (throwing rocks at buttons: the batarang's targets), climbs and, at the end,
the blind prisoner's "Rise!"; quest 603 completed, its win screen, and `RequireLoadLevel` to Gotham's spawn 600101,
where quest 838 completes and the main story moves on to 301 (`under_pressure_intro`). What it needed:

- **The level start**: TDKR's spawn points (`CSpawnPointObject::Create`) carry their model at `[14]` and mark the
  level start at `[16]` (Scene already started there; the scripts' start spawn now agrees). The player's model is the
  start spawn's.
- **Awareness** (`CAwarenessComponent` `0x6754056B`: `[0]` sight distance, `[4]` near distance, `[12]` seconds to spot,
  `[13]` field of view; gunmen 15 m / 180 degrees / 1.2 s, thugs 11 m / 160, snipers 60 m / 10): an enemy set
  `Aggressive` watches and goes for Batman only once it has seen him that long (or he is near); hits, noise and
  `NPCAttackTarget` start a fight at once. `ActorIsDetected(0)` is true while one is noticing him.
- **Default states**: the state set's names after its list are the defaults: `[1]` idle, `[2]` hurt, `[3]` death,
  `[5]` walk, `[6]` run (`Ranged_Idle`, `Ranged_Walk`, ...); actors start in `[1]` (the guards' scripts compare with
  `"Ranged_Idle"`) and walk with their own.
- `SetTakedownControlEnable` (the takedown button), `SetBurceWayneCombatStyle` (sic; recorded), `NPCGetStance`,
  `NPCGetTarget`, `IsEnabled`, `StopAnimation` / `SetAnimationSpeed`, `StopCameraAnim`, `PlayEffectOnObject` (the effect at
  the object's node plus an offset), `StartFadeOutWhite`, `PauseSound` (here a fade out), `MissionFailed` (back to the
  checkpoint; the death screen's text is not shown). Breakables now play their death effect.

**Under Pressure** (quest 301, back in Gotham at spawn 600101): trigger 600022 calls `NextMission(600152)`, whose
`Obj600152_Init` plays the `under_pressure_intro` storyboard and the binoculars cinematic; then the couple and their
three thugs, the park where two policemen hold off thugs, three snipers on the rooftops (each with two thugs coming up
when Batman gets near), two Bane SUVs that drive in and drop three thugs each, and a Bane helicopter. Batman grapnels
onto it, hangs under it and plants the EMP; Blake talks, the trigger on Gordon's roof plays the radio call, the antenna
hack overhears Bane's men, and `SetQuestCompleted(301)` gives the Waynetech win screen and `UnloadMission(600152)`.
What it needed:

- **Mission zones** (`CZonesManager`): a zone record whose `[0]` is 1 is a mission zone, `[4]` its id, and the objects
  in its byte span belong to it. They are loaded only while it is the current mission: `NextMission(zone)` unloads the
  old one (`UnloadMission`) and loads the new one as the level placed it (enabled, visible, where it stood), then runs
  `Obj<zone>_Init`. An object in an unloaded zone is not there for the scripts (`FindObject` fails), and its scripts stop.
  Without this the tutorial's triggers (zone 900347) fired in the middle of quest 301.
- `SpawnObject` starts the new actor on its waypoint's chain (`CNpcAIComponent::StartPatrol`): its `OnReach` runs
  there and it goes on along the chain (the SUVs' route, the snipers' `NPCSetSniper`); it leaves the chain to fight.
- **The helicopter** (`CHelicopterLogic` `0x786F6ECB`): it flies its waypoints; `ActorGrapnelEnable(heli, true)` makes
  it a grapnel target, and the grapnel takes Batman up to hang under it (`Grapnel_Fly_To_Grab_Heli`, `Grab_Heli_Idle`)
  instead of pulling it. That is the AI controller's `On_Actor_Attached` (`Templ9801_<behaviour>_<event>`, the
  behaviour being `CNpcAIComponent` `[0]`: 5 for helicopters), which enables its interaction; using it runs the type's
  `OnScriptEvent`: `UnlinkObject(0)` lets go, `KillActor(heli, "Explode")`. Its guns are not done.
- `NPCStandGroundTarget(actor, target)` (target set, stance 4, turned to it), `NPCPatrolArea(actor, waypoint, radius)`
  (walks from point to point within the radius, pausing), `PlayerIsFighting()` (true and the target's id while Batman
  fights), `UnloadMission(zone)`. NPCs targeting other NPCs (the police at the thugs) are not Batman's enemies.

**The Power Station** (quest 601, after Under Pressure): Gordon calls; Batman meets him on a roof (a quest-giver map
icon, `AddGenericObjectOnMap(502396, "questGiver1")`), and within 7 m `NextMission(703128)` starts it: the briefing
cinematic, then three sub-generator panels (502381, 502383, 502423), each covered by two snipers and a gang, one by
an alarm post that arms electric turrets, and two surveillance cameras that call a van of thugs; the central fight,
the prisoner's talk, the guards on the cooling tower and Selina; "The Power Station" win screen, `UnloadMission`, and
the story moves on to quest 25 (Save Lucius). What it needed:

- **Checkpoints keep the scripts' globals.** The game's checkpoint writes Lua's `_G` (`CLuaScriptManager::SaveSave`,
  `WriteLuaTableToStream`) and a respawn reads it back over the freshly loaded scripts (`CLevel::RespawnInit`:
  `ResetUserDefinedGlobalVars`, `LoadCurrentProgress`). The power plant's `Running` lives on tables its `Accepted`
  builds (`c_PP_ObjectiveArray`); without them a death broke the quest. Numbers, strings, booleans and tables of them
  are kept (in the save file too); functions come from the chunks, and Lua's own libraries are left out. The
  checkpoint also keeps the ammo (`CInventory::SaveSaveGlobal`): a death no longer costs the batarangs thrown since.
- **Surveillance cameras** (`CSurveillanceCameraComponent` `0x62353C58`: `[0]` sweep degrees, `[6]` range in metres,
  `[7]` ms to spot, ..., the `OnCamDetectTarget` script): a camera that sees Batman within its cone for that long runs
  `Obj<id>_OnCamDetectTarget` (`OnCamLostTarget` when he is gone); `DisableSurveillanceCamera` switches it off, a
  batarang breaks it (1 health), and the checkpoint keeps which are off. The lens looks along the model's +X turned by
  its yaw (camera 502387 at 109 degrees faces its panel). The sweep, the `camera` node turning, is not done yet.
- **Groups** (`CGroupObject`, the builtin `c c [members]`): `GetWaypointsInGroup` / `GetGroupsInGroup` return a
  group's own members that are waypoints / groups (the bomb side missions' spawn sets).
- `NPCSavePosition` (where `NPCReturnToSavedPosition` goes back to), `ShowCoronas`, `CameraResetFOV` (70 degrees),
  `ExitTheBat` (as `ExitBatPod`). `GenerateDistrict` and `ClearCrimeAreaMessage` are accepted: the crime hot spots
  (`CCrimeHotSpotComponent`) are not played yet.

**Save Lucius** (quest 25, after the Power Station): Gordon's call, a quest giver, `NextMission(900175)`; three
interrogations (a beaten thug, a gang boss, a man hung off the Zebra building), each after a fight; then the glide to
Wayne Tower over exhaust fans, unseen by its guards (landing short at trigger 900069 fails the mission), and Lucius
held at gunpoint on the roof; the win screen, and the story moves on to quest 816 (the military base). What it needed:

- `SetInteractionParamsProgress(object, radius, target, button frame, ms, text, win text, lose text)`
  (`CInteractionComponent::SetInteractionParams` with a hold time): the use takes `ms` (the C4 disarms 3-5 s, the
  interrogations 0), running while Batman stays in reach and is not attacked; let go, it starts over. The progress
  bar is not drawn yet.
- **Exhaust fans** (`CExhaustFanComponent` `0x65DA04ED`: `[3]` starts off, `[4]` radius, `[5]` push lost per metre
  up, `[6]` push at the fan, `[7]` column height; placed 4 / 2 / 120-130 / 60-65): the level sums the pushes where
  Batman is (`CLevel` `+0xb10`), a glide rises by them, and a jump in one of 2 and up launches him up the column
  (`CanGlideJump`, `GlideJump`) until the lift no longer beats the glide's sink, then he glides off where he looks.
  The push's unit is not known: a lift of 0.15 m/s per unit carries him some 50 m up a fan. `EnableExhaustFan`.
- **Checkpoints keep the quests** too (`CLevel::SaveGlobalData`: `CQuestManager::SaveSaveGlobal`, the main story's
  place): a failed mission comes back at its checkpoint still running, with its globals, instead of starting over.
- `MoveGroup(group, destination, reference)` (`CGroupObject::MoveGroup`: the members, sub-groups and waypoints moved
  so the reference lands on the destination, turned by the heading difference; the destination is the reference
  next time), `RemoveNPCOnRestart` (a respawn here drops spawned actors anyway).

**Sound and music** (`engine/src/game/tdkr_sound_api.cpp`): `sounds.xml` (VoxSoundPackXML: sounds, groups, banks and
events) is the sound table; `StartMusicEvent(event)` stops the music and plays the event, `StartSoundWithState` /
`SetSoundState`, `StartSoundAtObjectNode` / `StartEventAtObjectNode` play at an object and follow it
(`StopCurrentSFXAtObject` stops them), `ShowMessage` also plays the line's voice (the string id without `STR_`),
`KillMusic` stops the music. `PlayAmbience(preset)` plays the preset's sounds from `BATMAN_TDKR.vxa` at random
intervals: "voxa", a version string, "data", size, count, then per preset its name, u32 0, u32 ?, f32 min and max
seconds, two f32, the sound count and its sounds (u32 length, name, u32 0). Reverb presets, wet footsteps and the
combat music switch are not done yet.

**Controller prompts** (Android 1.1.6, `ShowObjectiveMessage`): with a PowerA pad connected, ids starting
`STR_TUTORIAL`, `STR_SAVE_LUCIUS_OBJ_6` and `STR_TURBULENCE_THIEF_MINIGAME` get `_POWER_A` inserted after that
prefix (an Xperia Play gets `_XPERIA`, or `_XPERIA_JP` in Japan). The PowerA layout is the Xbox one: A jump /
glide / climb, X attack, Y counter, B batarang, RT grapnel, LT gadget.

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
- Flash UI: the menu flow runs (title, main, chapters, pause, death), but Options, Extras, Help and the Tech Shop
  (`Shop.swf` through `SHOP_PARENT`) have no game side yet, chapter progress is not saved (all unlocked), and the
  chapter list does not scroll by keyboard. The HUD shows Batman's on-foot buttons; their states (counter, glide,
  takedown), health, XP, popups, objectives and the joystick for touch are still to be driven from the game.
  Colour transforms' add terms are ignored on bitmaps; non-convex masks clip to their hull; button hit tests use
  bounds.
- The Batpod, Tumbler and Bat vehicles have no N.O.V.A. 3 counterpart.
- Lights: no level uses the light record (0x265E); lighting objects use other components (`CHasLightComponent`, `CSoundAndLight`).
