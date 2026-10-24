"""Print every Lua chunk of a level (or of the object library), each under a line naming its
object: index, kind and type name.

    python3 tools/level_scripts.py work/extracted/data/game_config/GothamCity.lvc > gotham.lua
    python3 tools/level_scripts.py --gol work/extracted/data/game_config/gol.bin > gol.lua
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lvc  # noqa: E402


def chunks(value, out):
    if isinstance(value, str):
        if "function " in value or "\n" in value:
            out.append(value)
    elif isinstance(value, dict):
        for v in value.values():
            chunks(v, out)
    elif isinstance(value, (list, tuple)):
        for v in value:
            chunks(v, out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    if argv[1] == "--gol":
        gol = lvc.load_gol(argv[2])
        for tid, t in gol.items():
            found = []
            chunks(t.templates, found)
            for c in found:
                print(f"-- ==== gol {tid} {t.name}\n{c}")
        return 0
    path = Path(argv[1])
    gol = lvc.load_gol(str(path.with_name("gol.bin")))
    level = lvc.Level(str(path), gol)
    for i, o in enumerate(level.objects):
        found = []
        chunks(o.data, found)
        for c in found:
            print(f"-- ==== #{i} {o.kind} {o.name}\n{c}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
