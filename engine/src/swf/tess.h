// Shapes to triangles: each fill style's edges cut into horizontal bands and filled even-odd
// as trapezoids (as gameswf does; Flash shapes are planar, crossings split a band), lines as
// quads along the flattened edges.
#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "swf/movie.h"

namespace tdkr::swf {

struct Mesh {
    const FillStyle* fill = nullptr;    // null for a line
    glm::vec4 line_color{0};
    std::vector<glm::vec2> triangles;   // shape space (twips), 3 per triangle
};

// `tolerance`: largest distance (twips) between a curve and its flattening; lines are at least
// `min_line_width` twips wide (hairlines: one pixel).
std::vector<Mesh> tessellate(const std::vector<ShapeGroup>& groups, float tolerance, float min_line_width);

// Append a quadratic curve's points after p0 (p0 itself not included).
void flatten(const Edge& e, float tolerance, std::vector<glm::vec2>& out);

}  // namespace tdkr::swf
