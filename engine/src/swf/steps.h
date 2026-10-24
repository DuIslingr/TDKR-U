// Scripted steps against a running movie (tdkr-check swf and tdkr --swf use them to reach a
// menu or HUD state): goto=PATH:LABEL  stop=PATH:LABEL  call=PATH:METHOD[:ARG...]  hide=PATH
// show=PATH  set=PATH:MEMBER:VALUE  text=PATH:TEXT  load=PATH:MOVIE  frames=N  tree=PATH[:DEPTH]
// click=X:Y  move=X:Y (screen pixels)
#pragma once

#include <string>

#include "swf/player.h"

namespace tdkr::swf {

// Run one step; false (with a message in `error`) if it is not understood.
bool run_step(Player& player, const std::string& step, std::string* error = nullptr);
// Print a clip's tree (names, kinds, frames, positions) to stdout.
void print_tree(Instance* inst, int depth, int max_depth);

}  // namespace tdkr::swf
