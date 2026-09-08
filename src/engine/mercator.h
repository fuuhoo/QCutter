// Web-Mercator 绝对级别金字塔 (对齐 gdal2tiles -p mercator 语义).
// 级别为全球 XYZ 绝对缩放级: z 级世界宽 256·2^z px;
// 每级瓦片数 = 与影像 3857 范围相交的全球网格单元数.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace qcutter {

inline constexpr double MERC_INIT_RESOLUTION = 156543.03392804062;
inline constexpr double MERC_ORIGIN_SHIFT = 20037508.342789244;

/// 某级别的全球网格瓦片范围（含端点）.
struct MercLevel {
    std::uint32_t z = 0;
    std::uint32_t tx0 = 0, ty0 = 0;
    std::uint32_t tx1 = 0, ty1 = 0;
    std::uint64_t count() const {
        return static_cast<std::uint64_t>(tx1 - tx0 + 1)
             * static_cast<std::uint64_t>(ty1 - ty0 + 1);
    }
};

struct MercPlan {
    std::uint32_t native_zoom = 0;
    std::vector<MercLevel> levels;
    std::uint64_t total_tiles = 0;
};

/// gdal2tiles ZoomForPixelSize: 最大的 z 使 Resolution(z) ≥ pixel_size.
std::uint32_t zoomForPixelSize(double pixel_size);

/// 规划 mercator 金字塔.
/// 对齐 gdal2tiles: 用户显式指定的级别范围不截断到 native;
/// 级别下限为 1（不生成 Z0）.
MercPlan planMercator(std::array<double, 4> bounds3857,
                      double sx_m,
                      std::uint32_t tile,
                      std::optional<std::uint32_t> req_min,
                      std::optional<std::uint32_t> req_max,
                      std::uint64_t max_total);

} // namespace qcutter