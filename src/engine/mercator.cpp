#include "engine/mercator.h"
#include "engine/error.h"
#include <algorithm>

namespace qcutter {

std::uint32_t zoomForPixelSize(double pixel_size) {
    for (std::uint32_t i = 0; i <= 30; ++i) {
        const double res = MERC_INIT_RESOLUTION / std::pow(2.0, static_cast<int>(i));
        if (pixel_size > res) {
            return (i == 0) ? 0 : i - 1;
        }
    }
    return 30;
}

static MercLevel levelRange(const std::array<double, 4>& bounds,
                            std::uint32_t z, std::uint32_t tile) {
    const double res = MERC_INIT_RESOLUTION / std::pow(2.0, static_cast<int>(z));
    const double t = static_cast<double>(tile);
    const auto [minx, miny, maxx, maxy] = bounds;
    const auto px = [&](double mx) { return (mx + MERC_ORIGIN_SHIFT) / res; };
    const auto py = [&](double my) { return (MERC_ORIGIN_SHIFT - my) / res; };
    const double fx0 = std::floor(px(minx) / t);
    const double fy0 = std::floor(py(maxy) / t);
    const double fx1 = std::floor(px(maxx) / t);
    const double fy1 = std::floor(py(miny) / t);
    MercLevel lv;
    lv.z = z;
    lv.tx0 = static_cast<std::uint32_t>(std::max(0.0, fx0));
    lv.ty0 = static_cast<std::uint32_t>(std::max(0.0, fy0));
    lv.tx1 = static_cast<std::uint32_t>(std::max(0.0, fx1));
    lv.ty1 = static_cast<std::uint32_t>(std::max(0.0, fy1));
    return lv;
}

MercPlan planMercator(std::array<double, 4> bounds3857,
                      double sx_m,
                      std::uint32_t tile,
                      std::optional<std::uint32_t> req_min,
                      std::optional<std::uint32_t> req_max,
                      std::uint64_t max_total) {
    const std::uint32_t nz = zoomForPixelSize(std::abs(sx_m));
    const std::uint32_t zmin = std::max(req_min.value_or(1u), 1u);
    const std::uint32_t zmax = std::max(req_max.value_or(nz), zmin);
    MercPlan plan;
    plan.native_zoom = nz;
    plan.levels.reserve(zmax - zmin + 1);
    for (std::uint32_t z = zmin; z <= zmax; ++z) {
        auto lv = levelRange(bounds3857, z, tile);
        plan.total_tiles += lv.count();
        if (plan.total_tiles > max_total) {
            throw CoreError::invalid("level range [" + std::to_string(zmin) + ", "
                                     + std::to_string(zmax) + "] estimated tiles exceeds "
                                     + std::to_string(max_total));
        }
        plan.levels.push_back(lv);
    }
    return plan;
}

} // namespace qcutter