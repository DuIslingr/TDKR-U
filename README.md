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
forward first, `--grapple-test SECONDS` to fire the grapnel after 1 s). In the window: mouse to look, WASD to move
Batman, Shift to sprint, Space to jump (again in the air to glide; while gliding W dives, Shift+W plunges), F to grapple,
E to use, the left / right buttons to strike / counter (the tutorial checks for them; there is no melee yet).
A gamepad works too, laid out like the Android build's PowerA pad: left stick moves, right stick looks, A jumps / glides,
left stick click sprints, right trigger grapples, X strikes, Y counters, B throws the batarang (Q on the keyboard),
Start pauses; in the menus the D-pad or left stick moves, A selects,
B goes back. With a pad connected (or `--gamepad`), tutorial prompts name its buttons.
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
auto-play also presses use (the Pit's valves stand beside their markers).
The HUD is the game's own `BatmanHUD.swf`. Without `--level` the game starts on its title screen and main menu
(arrows / WASD and Enter, or the mouse; Escape goes back); Escape in a level opens the pause menu.
`--front-end SCREEN[:KEYS]` screenshots a menu headless (`title`, `main`, `pause`, `death`; keys `u d l r a b .`).

The Flash UI: `python3 tools/swf.py info|tree|actions|texts|render FILE.swf` inspects a movie;
`engine/build/tdkr-check swf work/data MainMenu.swf out.png --size 1280x720 call=screenManager:startScreen:main frames=60`
plays one headless (steps: `goto=`, `stop=`, `call=`, `hide=`, `show=`, `set=`, `text=`, `load=`, `frames=`, `tree=`;
text runs show as boxes there). In the game, `--swf MainMenu.swf --swf-step call=screenManager:startScreen:main`
shows a movie over the level (the mouse drives it; Escape quits), also with `--screenshot`.
