// TIFF/GeoTIFF 元数据探测.
#pragma once
#include <array>
#include <cstdint>
#include "util/fs_compat.h"
#include <optional>
#include <string>
#include <vector>

namespace qcutter {

/// GeoTIFF 地理参考. 坐标统一到 EPSG:3857, sx > 0, sy < 0 (北向上).
struct GeoRef {
    double mx0 = 0;       ///< 左上角 X (EPSG:3857 米)
    double my_top = 0;    ///< 左上角 Y (EPSG:3857 米, 北界, 较大值)
    double sx = 0;        ///< 像素宽度 (米, 正值)
    double sy = 0;        ///< 像素高度 (米, 负值)
    std::uint32_t src_epsg = 3857; ///< 源投影 EPSG (用于精确反算)
    double src_tie_x = 0; ///< 源 tiepoint X (原始投影坐标)
    double src_tie_y = 0; ///< 源 tiepoint Y (原始投影坐标)
    double src_px_w = 0;  ///< 源像素宽度 (原始投影单位, 正值)
    double src_px_h = 0;  ///< 源像素高度 (原始投影单位, 正值)

    /// 返回 EPSG:3857 bounds [minx, miny, maxx, maxy].
    std::array<double, 4> bounds3857(std::uint32_t w, std::uint32_t h) const {
        const double x1 = mx0 + static_cast<double>(w) * sx;
        const double y_south = my_top + static_cast<double>(h) * sy;
        return { std::min(mx0, x1), y_south, std::max(mx0, x1), my_top };
    }
};

/// GeoTIFF 探测 (PixelScale + Tiepoint + GeoKey EPSG).
/// 无地理参考时返回 nullopt.
std::optional<GeoRef> probeGeoref(const fs::path& path);

/// GeoKeyDirectory (tag 34735) 中解析 EPSG.
std::optional<std::uint32_t> parseGeoKeyEpsg(const std::vector<std::uint32_t>& keys);

/// 注册 GeoTIFF 自定义 tags 到 libtiff (33550/33922/34735/34737/42113).
/// 必须在任何 TIFFOpen 之前调用; 全局只生效一次, 可重复调用.
void installGeorefTags();

} // namespace qcutter