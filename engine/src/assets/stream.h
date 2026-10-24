// Streamed level geometry (CLevelStreaming): Gotham's buildings, roads and bridges, kept in
// l_gothamcity/<Level>[_low_inorm].zip instead of being placed as level objects. See
// tools/streaming.py for the format. The whole city is turned into one model at one LOD.
#pragma once

#include "assets/bdae.h"
#include "core/bytes.h"

namespace tdkr {

// Build a model from a streaming archive (the .zip's bytes): its materials come from the
// archive's <Level>_materials.bdae, one mesh per batch holding that batch's segments at `lod`
// (clamped per group; 0 is the most detailed), all at one root node in world space.
Model load_level_stream(const Bytes& zip, int lod = 0);

}  // namespace tdkr
