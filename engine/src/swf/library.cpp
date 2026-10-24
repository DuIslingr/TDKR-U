#include "swf/library.h"

#include <cstdint>

#include "core/vfs.h"

namespace tdkr::swf {

Library::Library(const Vfs& vfs, std::vector<std::string> resolutions) : vfs_(vfs), resolutions_(std::move(resolutions)) {}

uint32_t Library::pick(const std::string& name) const {
    const auto& hits = vfs_.find(name);
    if (hits.empty()) return UINT32_MAX;
    for (const std::string& res : resolutions_)
        for (uint32_t e : hits)
            if (vfs_.archive_path(vfs_.entry(e).archive).filename().string().find(res) != std::string::npos) return e;
    return hits.front();
}

std::shared_ptr<MovieDef> Library::movie(const std::string& file) {
    std::string name = file;
    if (auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() < 4 || to_lower(name.substr(name.size() - 4)) != ".swf") name += ".swf";
    const std::string key = to_lower(name);
    if (auto it = movies_.find(key); it != movies_.end()) return it->second;
    const uint32_t e = pick(name);
    std::shared_ptr<MovieDef> m;
    if (e != UINT32_MAX) {
        m = load_movie(vfs_.read(e), name.substr(0, name.size() - 4));
        archive_[m.get()] = vfs_.entry(e).archive;
    }
    movies_[key] = m;
    return m;
}

const Image* Library::bitmap(const MovieDef& movie, uint16_t id) {
    const std::string name = movie.name + "_atlas" + std::to_string(id) + ".tga";
    const std::string key = to_lower(name) + "#" + std::to_string(archive_.count(&movie) ? archive_[&movie] : 0);
    if (auto it = images_.find(key); it != images_.end()) return it->second.get();
    std::unique_ptr<Image> img;
    const auto& hits = vfs_.find(name);
    uint32_t chosen = UINT32_MAX;
    if (auto a = archive_.find(&movie); a != archive_.end())
        for (uint32_t e : hits)
            if (vfs_.entry(e).archive == a->second) chosen = e;
    if (chosen == UINT32_MAX && !hits.empty()) chosen = hits.front();
    if (chosen != UINT32_MAX) {
        img = std::make_unique<Image>(load_texture(vfs_.read(chosen)));
    } else if (const CharacterDef* c = movie.character(id); c && c->kind == CharacterDef::Bitmap) {
        const auto* b = static_cast<const BitmapDef*>(c);
        if (!b->rgba.empty()) {
            img = std::make_unique<Image>();
            img->width = b->width;
            img->height = b->height;
            img->rgba = b->rgba;
        }
    }
    return (images_[key] = std::move(img)).get();
}

}  // namespace tdkr::swf
