# The Dark Knight Rises reimplementation

A native desktop reimplementation of Gameloft's *The Dark Knight Rises* (2012), forked from the
N.O.V.A. 3 one (`../N.O.V.A 3`): the same Glitch engine, so the archive, texture, model, shader and
Lua code carries over. Behaviour comes from the Android library (`libKRHP.so`, which ships with its
symbols) in Ghidra. Findings so far are in [ANALYSIS.md](ANALYSIS.md).

**The game's files are not included.** You need your own copy of the game data. The default is the
Android build's (Gameloft's Amazon 1.1.6 data, as in `DarkKnightRises.apk`'s `assets/gamedata.zip`): it is
what the gameplay footage shows and carries the controller prompts. The iOS build (1.0.4: the `.app`
folder from the IPA) works too.

## Layout

| Path | What |
| --- | --- |
| `engine/` | The engine (C++20, SDL3). Forked from N.O.V.A. 3; so far the archive, texture and model loaders handle TDKR's data |
| `engine/tools/check.cpp` | `tdkr-check`: decoder checks over a data folder (`swf_check.cpp`: play a Flash movie to PNG) |
| `engine/src/swf/` | The Flash player for the gameswf UI (parser, tessellator, ActionScript 2, display list) |
| `tools/` | Python reference decoders (`gla.py`, `textures.py`, `bdae.py`, `bdae2gltf.py`, `swf.py`), symbol matching, Ghidra scripts |
| `work/` | Local only (gitignored): extracted data, Ghidra projects, decompiled code |

## Building (Linux)

With the libraries from the system (Arch: `sdl3 sdl3_ttf zlib lua51 shaderc glm`):

```bash
cmake -S engine -B engine/build -G Ninja
cmake --build engine/build
```

The other platforms build as N.O.V.A. 3 does (`-DTDKR_VENDORED=ON`, `-DTDKR_SHADERCROSS=ON`).

## Controls

The gamepad follows the Android build's own controller layout (the PowerA pad's, an Xbox layout), as its
`_POWER_A` tutorial prompts and loading hints give it; the keyboard and mouse are this port's.

| Action | Gamepad | Keyboard and mouse |
| --- | --- | --- |
| Move; steer a glide (up dives) | left stick | W A S D |
| Look (the camera) | right stick | mouse |
| Sprint | left stick click | Shift |
| Jump, climb up; in the air, glide; on an exhaust fan, ride its updraft | A | Space |
| Use (switches, hostages, the bomb disarms, talking); get off the Batpod | A, near something usable | E |
| Attack; a leap takedown from above | X | left button |
| Block and strike back (counter) | Y | right button |
| Batarang | B | Q |
| Grapnel (an edge, or pulls an enemy; onto a helicopter) | right trigger | F |
| Gadgets | left trigger (the gadget wheel is not done yet) | - |
| Answer a QTE button | A or X | E, F or a click |
| Batpod: throttle / brakes, steer | left stick | W / S, A / D |
| Pause | Start | Escape |
| Skip a storyboard | any button | any key or click |
| Win screen: continue; death and level-end screens | A; D-pad to move | Enter, Space or a click; arrows |
| Menus | D-pad or left stick to move, A selects, B goes back | arrows or WASD, Enter, Escape; or the mouse |

A held use (the C4 disarms) runs on its own once started, under its progress bar, while Batman stays in reach and is
not attacked. The hacking game is played with the mouse or a finger. With a pad connected (or `--gamepad`), the
tutorial prompts name its buttons. `--touch` plays with the original's touch controls instead.

## Display

Options → Display (in the main menu's and the pause menu's Flash options, and the C++ pause menu's Display page) holds
this port's display settings, saved with the others:

| Setting | Values |
| --- | --- |
| Resolution | 16:9, 1280x720 up to 3840x2160 |
| Display mode | windowed, fullscreen, borderless |
| V-Sync | on, off |
| Frame rate limit | 30, 45, 60, 75, 90, 120 fps or unlocked. The game's logic steps at a fixed 120 Hz whatever the frame rate, so it never runs faster or slower |
| Upscaler | off; AMD FSR 1 (spatial); AMD FSR 3 (temporal, Vulkan); NVIDIA DLSS and Apple MetalFX are listed but not built yet (see below) |
| Upscaler mode | Native AA (FSR 3's anti-aliasing at full size; FSR 1 only sharpens), Quality (1.5x), Balanced (1.7x), Performance (2x), Ultra Performance (3x) |
| Sharpness | 0-100% (RCAS) |

With an upscaler, the world and its post effects are drawn at the smaller size and the HUD at full size. FSR 3
jitters the camera by a sub-pixel each frame and accumulates the frames with the depth and motion vectors: the
camera's motion everywhere, plus what moved on its own (skinned characters, moving objects). Its passes are AMD's
FidelityFX SDK shaders as shipped (`engine/third_party/fidelityfx`, MIT; see its `LICENSE.txt`), compiled to SPIR-V
compute for SDL's GPU API. DLSS and MetalFX need the native Vulkan / Metal objects behind SDL's GPU API, which SDL
3.4 does not hand out, plus NVIDIA's SDK (DLSS) or a Mac (MetalFX).

For a run without changing the settings: `--fps-limit N` (0 unlocked), `--upscaler off|fsr1|fsr3|dlss|metalfx`,
`--upscale-mode native|quality|balanced|performance|ultra`, `--sharpness 0..1`. `--window-test SECONDS` plays in a
window and reports game time against wall time. Headless, `--shot-frames N` draws the run's last N frames (or, with
`--shot-turn YAW[,PITCH]`, the shot turned that many degrees a frame) so a temporal upscaler's screenshot has its
history; `TDKR_FSR3_DEBUG=1` shows FSR 3's own debug view (motion, masks, depth). The touch controls' buttons and
joystick are hidden unless `--touch`.

## Checking the data

```bash
engine/build/tdkr-check vfs work/data
engine/build/tdkr-check textures work/data
engine/build/tdkr-check models work/data models.txt
```

`work/data` is the Android data: `gamedata.zip` unpacked (`unzip -p DarkKnightRises.apk assets/gamedata.zip >
gamedata.zip`, then `unzip gamedata.zip 'data/*' 'textures/*' -d work/data`); its `data/` and `textures/`
folders are mounted. `work/extracted/data` holds its archives unpacked for the Python tools (`python3 tools/gla.py
extract --per-archive -o work/extracted/data work/data/data/*.gla work/data/textures/*.gla`). The iOS data works
the same way from `work/data_ios` (the IPA's `Payload/TDKR.app`) and `work/extracted/data_ios`; pass
`--data work/data_ios` to the engine.
To preview a model: `python3 tools/bdae2gltf.py MODEL.bdae -o out.glb --data work/extracted/data`.
To read a level's scripts: `python3 tools/level_scripts.py work/extracted/data/game_config/GothamCity.lvc` (each
chunk under its object; `--gol gol.bin` for the object library's). To check the levels:
`python3 tools/lvc.py work/extracted/data/game_config/*.lvc`; to preview one:
`python3 tools/level2gltf.py work/extracted/data/game_config/Stadion.lvc -o stadion.glb` (Gotham adds its streamed city;
`tools/streaming.py` exports just that).

To run a level in the engine: `engine/build/tdkr --data work/data --level Stadion.lvc --no-scripts` (add
`--screenshot OUT.bmp` for one frame, `--eye X,Y,Z,YAW,PITCH` to place the camera, `--walk-test SECONDS` to walk
forward first, `--grapple-test SECONDS` to fire the grapnel after 1 s). The controls are under
[Controls](#controls).
`--mission N|NAME` starts a mission as the menus do (its level, then its start script); `--list-missions` lists them.
The level scripts run on the game's own script API (`engine/src/game/tdkr_api.cpp`; `--trace-scripts` logs what they
start and, headless, counts the calls to natives not implemented yet). `--touch-test SECONDS` swipes and walks at
the end of a headless run; `--did look,move` marks those done from the start, `--fight-test` strikes, counters,
takes down and fires a locked grapnel by itself, and `--exec-at T:LUA` runs a script line at T seconds (e.g.
`--exec-at "75:ObjectSetPosition(0,-478.7,-970.5,26.2)"` to stand somewhere).
`--auto-play` (with `--fight-test`) also walks Batman to each objective marker in turn and uses it (or, with no
marker, the nearest usable thing), answers QTE buttons and wins the hacking game: the Stock Exchange plays through
headless with `--level StockExchange.lvc --exec "TeleportPlayerToZone(701998)" --auto-play`. In the hacking game
the mouse turns the circles. On the Batpod (quest 1000) W / S (or the left stick) are the throttle and the brakes, A / D steer, and
E gets off; `--auto-play` drives it after the objective, tailing a moving one from 40-50 m back. The Underground plays through
the same way (`--level Underground.lvc --spawn-at 700002 --exec "StartMainStoryFromQuestId(838)" --exec
'SetWeaponAmmo("Batarang_01", 10)' --auto-play`); auto-play breaks the objectives' lights with the batarang. A
storyboard skips on a key or click. The Pit too (`--level ThePit.lvc --auto-play`); at a place marker
auto-play also presses use (the Pit's valves stand beside their markers). Quest 301 (Under Pressure) plays through in
Gotham with `--level GothamCity.lvc --spawn-at 600101 --exec "StartMainStoryFromQuestId(301)" --exec
'SetWeaponAmmo("Batarang_01", 10)' --auto-play`; auto-play stands beside an enemy on its own floor (the snipers'
ledges) and grapnels onto the helicopter from below it, then uses it. It takes the markers in a player's order:
enemies, then objectives, then the rest (hostages, people to talk to); with no marker it goes to a quest giver on the
map (Gordon, for quest 601: the same command with `StartMainStoryFromQuestId(601)`). At an exhaust fan marker it
stands on the fan and jumps up its updraft, steers the glide through the fans on the way, and over one it has not
used drops onto it to go up again (quest 25, `StartMainStoryFromQuestId(25)`). The military base follows from
`StartMainStoryFromQuestId(816)`; auto-play leaves optional security camera markers for last. In a glide, jump on a fan's top to
ride its updraft.
The HUD is the game's own `BatmanHUD.swf`. Without `--level` the game starts on its title screen and main menu
(arrows / WASD and Enter, or the mouse; Escape goes back); Escape in a level opens the pause menu.
`--front-end SCREEN[:KEYS]` screenshots a menu headless (`title`, `main`, `pause`, `death`; keys `u d l r a b .`).

The Flash UI: `python3 tools/swf.py info|tree|actions|texts|render FILE.swf` inspects a movie;
`engine/build/tdkr-check swf work/data MainMenu.swf out.png --size 1280x720 call=screenManager:startScreen:main frames=60`
plays one headless (steps: `goto=`, `stop=`, `call=`, `hide=`, `show=`, `set=`, `text=`, `load=`, `frames=`, `tree=`;
text runs show as boxes there). In the game, `--swf MainMenu.swf --swf-step call=screenManager:startScreen:main`
shows a movie over the level (the mouse drives it; Escape quits), also with `--screenshot`.
