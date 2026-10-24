#include "game/missions.h"

#include <algorithm>
#include <cstdio>

#include "core/vfs.h"

namespace tdkr {

std::vector<Mission> load_missions(const Vfs& vfs, const Gol& gol) {
    std::vector<Mission> out;
    std::vector<std::string> files;
    for (size_t i = 0; i < vfs.entry_count(); ++i) {
        std::string n = vfs.entry(uint32_t(i)).name;
        if (auto slash = n.find_last_of("/\\"); slash != std::string::npos) n = n.substr(slash + 1);
        const std::string low = to_lower(n);
        if (low.size() > 4 && low.substr(low.size() - 4) == ".lvc" && low != "test.lvc" && low != "shop.lvc" && low != "l_menu.lvc" &&
            std::find(files.begin(), files.end(), n) == files.end())
            files.push_back(n);
    }
    std::sort(files.begin(), files.end());
    for (const std::string& file : files) {
        Level level;
        try {
            level = load_level(vfs.read(file), gol);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "missions: %s: %s\n", file.c_str(), ex.what());
            continue;
        }
        for (const LevelObject& o : level.objects) {
            if (o.kind != "level_properties" || o.fields.size() <= 8) continue;
            for (const Value& e : o.fields[8].list) {
                if (e.size() < 15) continue;
                Mission m;
                m.level = file;
                m.script = e[0].s;
                m.name = e[1].s;
                m.menu_background = e[3].s;
                m.chapter = e[6].i;
                m.chapter_title = e[7].s;
                m.number = e[9].i;
                m.picture = e[10].s;
                m.selection = e[11].s;
                m.index_title = e[12].s;
                m.description = e[13].s;
                m.first = e[14].i != 0;
                out.push_back(std::move(m));
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Mission& a, const Mission& b) {
        const int ca = a.chapter > 0 ? a.chapter : 100, cb = b.chapter > 0 ? b.chapter : 100;
        return ca != cb ? ca < cb : a.number < b.number;
    });
    return out;
}

int first_mission(const std::vector<Mission>& missions) {
    for (size_t i = 0; i < missions.size(); ++i)
        if (missions[i].name == "STR_FPS_LEVEL_01_TUTORIAL") return int(i);
    for (size_t i = 0; i < missions.size(); ++i)
        if (missions[i].first) return int(i);
    return missions.empty() ? -1 : 0;
}

}  // namespace tdkr
