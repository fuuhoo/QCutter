// 相对模式金字塔规划: Google 风格金字塔, level 0 为整图一张瓦片, native level 为原始分辨率级.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qcutter {

inline constexpr std::uint32_t DEFAULT_TILE_SIZE = 256;
inline constexpr std::uint32_t MAX_LEVEL_CAP = 22;
inline constexpr std::uint64_t MAX_TOTAL_TILES_HARD = 100000000ULL; // 1亿

/// 瓦片编号方案。
enum class Scheme { Xyz, Tms };
inline std::string schemeToString(Scheme s) {
    return s == Scheme::Xyz ? "xyz" : "tms";
}
inline std::optional<Scheme> schemeFromString(const std::string& s) {
    if (s == "xyz") return Scheme::Xyz;
    if (s == "tms") return Scheme::Tms;
    return std::nullopt;
}

/// 重采样方式。
enum class Resample { Nearest, Bilinear };
inline std::string resampleToString(Resample r) {
    return r == Resample::Nearest ? "nearest" : "bilinear";
}

/// 单个金字塔级别的计划。
struct LevelPlan {
    std::uint32_t level = 0;
    std::uint32_t width = 0;   ///< 该级整幅影像尺寸（像素）
    std::uint32_t height = 0;
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;
    double scale = 1.0;        ///< 源像素 / 输出像素 (>1 缩小)
};

struct PyramidPlan {
    std::uint32_t image_width = 0;
    std::uint32_t image_height = 0;
    std::uint32_t tile_size = DEFAULT_TILE_SIZE;
    std::uint32_t max_level = 0;
    std::uint32_t min_level_requested = 0;
    std::uint32_t max_level_requested = 0;
    std::vector<LevelPlan> levels;
    std::uint64_t total_tiles = 0;
};

/// 计算 native level: 整图恰好放进一张瓦片之上、原始分辨率所在的最低级别.
inline std::uint32_t nativeLevel(std::uint32_t w, std::uint32_t h, std::uint32_t tile) {
    const std::uint32_t longest = std::max(w, h);
    if (longest == 0) return 0;
    const double ratio = static_cast<double>(longest) / std::max<std::uint32_t>(tile, 1);
    return static_cast<std::uint32_t>(std::max(0.0, std::ceil(std::log2(ratio))));
}

/// 某级别的显示尺寸与瓦片数. level > max_level 时按 2^n 上采样.
inline std::pair<std::uint32_t, std::uint32_t>
levelDims(std::uint32_t w, std::uint32_t h, std::uint32_t level, std::uint32_t max_level) {
    if (level <= max_level) {
        const auto ds = 1u << std::min<std::uint32_t>(max_level - level, 31);
        return { (w + ds - 1) / ds, (h + ds - 1) / ds };
    }
    const auto up = static_cast<std::uint64_t>(1) << std::min<std::uint32_t>(level - max_level, 24);
    const auto nw = std::min<std::uint64_t>(static_cast<std::uint64_t>(w) * up, 0xFFFFFFFFULL);
    const auto nh = std::min<std::uint64_t>(static_cast<std::uint64_t>(h) * up, 0xFFFFFFFFULL);
    return { static_cast<std::uint32_t>(nw), static_cast<std::uint32_t>(nh) };
}

/// 规划相对模式金字塔。请求级别范围允许 [0, MAX_LEVEL_CAP], 可超出 native (放大).
/// 总量超 MAX_TOTAL_TILES_HARD 时返回异常.
PyramidPlan planPyramid(std::uint32_t image_width,
                        std::uint32_t image_height,
                        std::uint32_t tile_size,
                        std::optional<std::uint32_t> min_level_req,
                        std::optional<std::uint32_t> max_level_req);

} // namespace qcutter