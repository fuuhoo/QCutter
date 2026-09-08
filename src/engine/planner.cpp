#include "engine/planner.h"
#include "engine/error.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace qcutter {

PyramidPlan planPyramid(std::uint32_t image_width,
                        std::uint32_t image_height,
                        std::uint32_t tile_size,
                        std::optional<std::uint32_t> min_level_req,
                        std::optional<std::uint32_t> max_level_req) {
    if (image_width == 0 || image_height == 0) {
        throw CoreError::invalid("image dimensions are zero");
    }
    if (tile_size < 64 || tile_size > 1024 || (tile_size & (tile_size - 1)) != 0) {
        throw CoreError::invalid("tile size must be power-of-two in [64, 1024]");
    }
    const auto max_level = nativeLevel(image_width, image_height, tile_size);
    const auto req_min = min_level_req.value_or(0);
    const auto req_max = max_level_req.value_or(max_level);
    if (req_min > req_max) {
        throw CoreError::invalid("level range invalid: [" + std::to_string(req_min) + ", "
                                 + std::to_string(req_max) + "]");
    }
    if (req_min > MAX_LEVEL_CAP) {
        throw CoreError::invalid("min level " + std::to_string(req_min)
                                 + " exceeds cap " + std::to_string(MAX_LEVEL_CAP));
    }
    const std::uint32_t zmin = req_min;
    const std::uint32_t zmax = std::min(req_max, MAX_LEVEL_CAP);
    const std::uint32_t zmax_eff = std::max(zmin, zmax);

    // 先估算总量, 超硬上限直接拒绝.
    {
        std::uint64_t total = 0;
        for (std::uint32_t lv = zmin; lv <= zmax_eff; ++lv) {
            const auto [w, h] = levelDims(image_width, image_height, lv, max_level);
            total += static_cast<std::uint64_t>((w + tile_size - 1) / tile_size)
                   * static_cast<std::uint64_t>((h + tile_size - 1) / tile_size);
            if (total > MAX_TOTAL_TILES_HARD) {
                throw CoreError::invalid("level range [" + std::to_string(zmin) + ", "
                                         + std::to_string(zmax_eff) + "] estimated tiles "
                                         + std::to_string(total) + " exceeds hard cap "
                                         + std::to_string(MAX_TOTAL_TILES_HARD));
            }
        }
    }

    PyramidPlan plan;
    plan.image_width = image_width;
    plan.image_height = image_height;
    plan.tile_size = tile_size;
    plan.max_level = max_level;
    plan.min_level_requested = zmin;
    plan.max_level_requested = zmax_eff;
    plan.levels.reserve(zmax_eff - zmin + 1);
    for (std::uint32_t lv = zmin; lv <= zmax_eff; ++lv) {
        const auto [w, h] = levelDims(image_width, image_height, lv, max_level);
        const auto tx = (w + tile_size - 1) / tile_size;
        const auto ty = (h + tile_size - 1) / tile_size;
        const auto exp = static_cast<int>(max_level) - static_cast<int>(lv);
        const double scale = std::pow(2.0, std::clamp(exp, -30, 30));
        plan.total_tiles += static_cast<std::uint64_t>(tx) * ty;
        LevelPlan lp;
        lp.level = lv;
        lp.width = w;
        lp.height = h;
        lp.tiles_x = tx;
        lp.tiles_y = ty;
        lp.scale = scale;
        plan.levels.push_back(lp);
    }
    return plan;
}

} // namespace qcutter