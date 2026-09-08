// 投影引擎: 纯 C++ 实现 EPSG → 任意投影坐标 → EPSG:3857 的转换。
// 不依赖 libproj, 直接数学公式 (UTM / 高斯-克吕格 / Mercator / 经纬度).
// 支持中国常用 EPSG: 32601-32660 (UTM 北), 32701-32760 (UTM 南),
// 4502-4512 (CGCS2000 6°GK), 4513-4533 (CGCS2000 3°GK),
// 2327-2337 (西安80 6°GK zone), 2338-2342 (西安80 6°GK CM),
// 2362-2369 (西安80 3°GK zone), 2370-2384 (西安80 3°GK CM),
// 2401-2421 (北京54 3°GK zone), 2422-2427 (北京54 3°GK CM),
// 以及 4326 (WGS84 经纬度), 3857 (Web Mercator).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace qcutter {

/// 椭球参数。
struct Ellipsoid {
    double a;       // 长半轴 (m)
    double f;       // 扁率
    constexpr double e2() const { return 2.0 * f - f * f; }                // 第一偏心率平方
    constexpr double e4() const { return e2() * e2(); }
    constexpr double e6() const { return e4() * e2(); }
};

/// 常用椭球 (与 PROJ 默认一致)。
namespace ellipsoid {
inline constexpr Ellipsoid WGS84()   { return { 6378137.0,         1.0/298.257223563 }; }
inline constexpr Ellipsoid GRS80()   { return { 6378137.0,         1.0/298.257222101 }; }
inline constexpr Ellipsoid IAU76()   { return { 6378140.0,         1.0/298.257 };       }
inline constexpr Ellipsoid Kras1940(){ return { 6378245.0,         1.0/298.3 };         }
} // namespace ellipsoid

/// 支持的投影族。
enum class ProjKind {
    LatLon,        // EPSG:4326 经纬度
    WebMercator,   // EPSG:3857
    Utm,           // UTM 北/南
    GaussKrZone,   // 中国 GK "zone 编号" (3° GK: 假东 (zone+0.5)*1M, 6° GK: 假东 (zone+0.5)*1M)
    GaussKrCm,     // 中国 GK "CM 命名" (假东 500000)
    Unknown,
};

struct ProjSpec {
    ProjKind kind = ProjKind::Unknown;
    Ellipsoid ell{};        // 椭球
    double lon_0 = 0.0;     // 中央经线 (度)
    double x_0 = 0.0;       // 假东 (m)
    double y_0 = 0.0;       // 假北 (m)
    double k_0 = 1.0;       // 比例因子
    bool south = false;     // UTM/GK 南半球假北 10000000
};

/// 从 EPSG 代码解析 ProjSpec. 不支持则返回 nullopt.
std::optional<ProjSpec> projSpecFromEpsg(std::uint32_t epsg);

/// 投影坐标 → EPSG:3857.
/// src_epsg=4326: x=lon度, y=lat度; 其它: 米.
/// 返回 false 表示不支持.
bool transformTo3857(std::uint32_t src_epsg, double x, double y, double& mx, double& my);

/// EPSG:3857 → 投影坐标.
/// src_epsg=4326: 输出 lon度, lat度; 其它: 米.
bool transform3857ToSrc(std::uint32_t src_epsg, double mx, double my, double& x, double& y);

/// 经纬度 → 3857 (内部).
bool lonLatTo3857(double lon_deg, double lat_deg, double& mx, double& my);

/// 3857 → 经纬度.
bool merc3857ToLonLat(double mx, double my, double& lon_deg, double& lat_deg);

} // namespace qcutter