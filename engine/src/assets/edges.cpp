#include "assets/edges.h"

namespace tdkr {

std::vector<GrappleEdge> load_grapple_edges(const Bytes& data) {
    const uint32_t n = read_be<uint32_t>(data, 0);
    check_range(data.size(), 4, size_t(n) * 48);
    std::vector<GrappleEdge> out(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto v = [&](int k) {
            const size_t o = 4 + size_t(i) * 48 + 12 * size_t(k);
            return glm::vec3(read_be<float>(data, o), read_be<float>(data, o + 4), read_be<float>(data, o + 8));
        };
        out[i] = {v(0), v(1), v(2), v(3)};
    }
    return out;
}

}  // namespace tdkr
