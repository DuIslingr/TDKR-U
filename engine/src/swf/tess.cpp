#include "swf/tess.h"

#include <algorithm>
#include <cmath>

namespace tdkr::swf {

void flatten(const Edge& e, float tolerance, std::vector<glm::vec2>& out) {
    if (!e.curve) {
        out.push_back(e.p1);
        return;
    }
    // A quadratic's distance from its n-segment chord polyline is |p0 - 2c + p1| / (8 n^2).
    const float dd = glm::length(e.p0 - 2.0f * e.control + e.p1);
    const int n = std::clamp(int(std::ceil(std::sqrt(dd / (8.0f * std::max(tolerance, 0.01f))))), 1, 64);
    for (int i = 1; i <= n; ++i) {
        const float t = float(i) / float(n), u = 1.0f - t;
        out.push_back(u * u * e.p0 + 2.0f * u * t * e.control + t * t * e.p1);
    }
}

namespace {

struct Seg {
    float x0, y0, x1, y1;   // y0 < y1
    float x_at(float y) const { return x0 + (x1 - x0) * (y - y0) / (y1 - y0); }
};

void emit_band(const std::vector<const Seg*>& active, float ya, float yb, std::vector<glm::vec2>& tris, int depth) {
    if (yb - ya < 1e-3f) return;
    struct Cross {
        float xa, xb, xm;
        const Seg* s;
    };
    std::vector<Cross> xs;
    xs.reserve(active.size());
    const float ym = (ya + yb) * 0.5f;
    for (const Seg* s : active) xs.push_back({s->x_at(ya), s->x_at(yb), s->x_at(ym), s});
    std::sort(xs.begin(), xs.end(), [](const Cross& a, const Cross& b) { return a.xm < b.xm; });
    // Edges crossing inside the band: split it at the first crossing.
    if (depth < 8) {
        float split = yb;
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
            const float da = xs[i + 1].xa - xs[i].xa, db = xs[i + 1].xb - xs[i].xb;
            if (da * db < 0) {
                const float t = da / (da - db);
                split = std::min(split, ya + (yb - ya) * t);
            }
        }
        if (split > ya + 1e-3f && split < yb - 1e-3f) {
            emit_band(active, ya, split, tris, depth + 1);
            emit_band(active, split, yb, tris, depth + 1);
            return;
        }
    }
    for (size_t i = 0; i + 1 < xs.size(); i += 2) {
        const Cross& l = xs[i];
        const Cross& r = xs[i + 1];
        const glm::vec2 a(l.xa, ya), b(r.xa, ya), c(r.xb, yb), d(l.xb, yb);
        if (r.xa - l.xa > 1e-4f) tris.insert(tris.end(), {a, b, c});
        if (r.xb - l.xb > 1e-4f) tris.insert(tris.end(), {a, c, d});
    }
}

void fill(const std::vector<Edge>& edges, float tolerance, std::vector<glm::vec2>& tris) {
    std::vector<Seg> segs;
    std::vector<glm::vec2> pts;
    for (const Edge& e : edges) {
        pts.clear();
        pts.push_back(e.p0);
        flatten(e, tolerance, pts);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            glm::vec2 a = pts[i], b = pts[i + 1];
            if (a.y == b.y) continue;
            if (a.y > b.y) std::swap(a, b);
            segs.push_back({a.x, a.y, b.x, b.y});
        }
    }
    if (segs.size() < 2) return;
    std::vector<float> ys;
    ys.reserve(segs.size() * 2);
    for (const Seg& s : segs) ys.push_back(s.y0), ys.push_back(s.y1);
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.y0 < b.y0; });
    std::vector<const Seg*> active;
    size_t next = 0;
    for (size_t i = 0; i + 1 < ys.size(); ++i) {
        const float ya = ys[i], yb = ys[i + 1];
        while (next < segs.size() && segs[next].y0 <= ya) active.push_back(&segs[next++]);
        active.erase(std::remove_if(active.begin(), active.end(), [&](const Seg* s) { return s->y1 <= ya; }), active.end());
        if (active.size() >= 2) emit_band(active, ya, yb, tris, 0);
    }
}

void stroke(const std::vector<Edge>& edges, float width, float tolerance, std::vector<glm::vec2>& tris) {
    const float hw = width * 0.5f;
    std::vector<glm::vec2> pts;
    for (const Edge& e : edges) {
        pts.clear();
        pts.push_back(e.p0);
        flatten(e, tolerance, pts);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const glm::vec2 a = pts[i], b = pts[i + 1];
            glm::vec2 d = b - a;
            const float len = glm::length(d);
            if (len < 1e-4f) continue;
            d /= len;
            const glm::vec2 n(-d.y * hw, d.x * hw);
            const glm::vec2 a0 = a - d * hw * 0.5f, b0 = b + d * hw * 0.5f;   // overlap a little at joins
            tris.insert(tris.end(), {a0 + n, b0 + n, b0 - n, a0 + n, b0 - n, a0 - n});
        }
    }
}

}  // namespace

std::vector<Mesh> tessellate(const std::vector<ShapeGroup>& groups, float tolerance, float min_line_width) {
    std::vector<Mesh> out;
    for (const ShapeGroup& g : groups) {
        for (size_t s = 0; s < g.fill_edges.size(); ++s) {
            if (g.fill_edges[s].empty()) continue;
            Mesh m;
            m.fill = &g.fills[s];
            fill(g.fill_edges[s], tolerance, m.triangles);
            if (!m.triangles.empty()) out.push_back(std::move(m));
        }
        for (size_t s = 0; s < g.line_edges.size(); ++s) {
            if (g.line_edges[s].empty()) continue;
            Mesh m;
            m.line_color = g.lines[s].color;
            stroke(g.line_edges[s], std::max(g.lines[s].width, min_line_width), tolerance, m.triangles);
            if (!m.triangles.empty()) out.push_back(std::move(m));
        }
    }
    return out;
}

}  // namespace tdkr::swf
