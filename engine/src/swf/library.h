// The game's Flash movies and their bitmap atlases from the archives. A movie comes in up to
// three resolutions (gameswf_hires / normalres / lowres archives, each with its own atlases);
// this takes the first of the preferred list that has it and reads its atlases from there.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/texture.h"
#include "swf/movie.h"

namespace tdkr {
class Vfs;
}

namespace tdkr::swf {

class Library {
public:
    explicit Library(const Vfs& vfs, std::vector<std::string> resolutions = {"gameswf_hires", "gameswf_normalres", "gameswf_lowres"});

    // "BatmanHUD.swf" (or without .swf); null if missing. Cached.
    std::shared_ptr<MovieDef> movie(const std::string& file);
    // The atlas image of a bitmap id (<movie>_atlas<id>.tga from the movie's archive), or the
    // SWF's own pixels; null if neither. Cached.
    const Image* bitmap(const MovieDef& movie, uint16_t id);
    const Vfs& vfs() const { return vfs_; }

private:
    uint32_t pick(const std::string& name) const;   // entry index, or UINT32_MAX
    const Vfs& vfs_;
    std::vector<std::string> resolutions_;
    std::unordered_map<std::string, std::shared_ptr<MovieDef>> movies_;
    std::unordered_map<const MovieDef*, uint32_t> archive_;   // movie -> archive it came from
    std::unordered_map<std::string, std::unique_ptr<Image>> images_;
};

}  // namespace tdkr::swf
