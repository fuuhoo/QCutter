#include "engine/proj_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace qcutter {

namespace {
inline constexpr double kPi = 3.14159265358979323846;
} // namespace

// 工具：度→弧度
inline constexpr double deg2rad(double d) noexcept { return d * kPi / 180.0; }
inline constexpr double rad2deg(double r) noexcept { return r * 180.0 / kPi; }

namespace {

// 经纬度 → 3857 (球面 Web Mercator 公式)
bool llTo3857Raw(double lon_deg, double lat_deg, double& mx, double& my) {
    constexpr double ORIGIN_SHIFT = 20037508.342789244;
    const double lat_rad = std::clamp(lat_deg, -85.0511287, 85.0511287) * kPi / 180.0;
    mx = lon_deg * ORIGIN_SHIFT / 180.0;
    my = std::log(std::tan(kPi / 4.0 + lat_rad / 2.0)) * ORIGIN_SHIFT / kPi;
    return true;
}

// 3857 → 经纬度
bool wm3857ToLlRaw(double mx, double my, double& lon_deg, double& lat_deg) {
    constexpr double ORIGIN_SHIFT = 20037508.342789244;
    lon_deg = mx * 180.0 / ORIGIN_SHIFT;
    const double lat_rad = 2.0 * std::atan(std::exp(my * kPi / ORIGIN_SHIFT)) - kPi / 2.0;
    lat_deg = lat_rad * 180.0 / kPi;
    return true;
}

// 高斯-克吕格正算 (BL→XY): 等角横轴圆柱投影, WGS84/GRS80/IAU76/北京54 椭球通用
// 输入 lat/lon 度; 输出 x/y 米 (含假东 x_0, 假北 y_0)
bool gkForward(const ProjSpec& p, double lat_deg, double lon_deg, double& x, double& y) {
    const double e2 = p.ell.e2();
    const double e4 = p.ell.e4();
    const double e6 = p.ell.e6();
    const double a  = p.ell.a;
    const double k0 = p.k_0;
    const double lon0 = p.lon_0;

    const double phi    = deg2rad(lat_deg);
    const double lam    = deg2rad(lon_deg);
    const double dlam   = lam - deg2rad(lon0);

    // 卯酉圈曲率半径相关参数
    const double sin_phi = std::sin(phi);
    const double cos_phi = std::cos(phi);
    const double tan_phi = std::tan(phi);

    const double N = a / std::sqrt(1.0 - e2 * sin_phi * sin_phi);
    const double T = tan_phi * tan_phi;
    const double C = e2 * cos_phi * cos_phi / (1.0 - e2);
    // 第二偏心率平方 e'² = e²/(1-e²)
    const double ep2 = e2 / (1.0 - e2);
    const double A = dlam * cos_phi;

    // 子午线弧长 (常数 M, 与球面相同的赤道至纬度 phi 弧长, 椭球校正)
    const double e4p = e4;
    const double term1 = (1.0 - e2/4.0 - 3.0*e4p/64.0 - 5.0*e6/256.0) * phi;
    const double term2 = (3.0*e2/8.0 + 3.0*e4p/32.0 + 45.0*e6/1024.0) * std::sin(2.0*phi);
    const double term3 = (15.0*e4p/256.0 + 45.0*e6/1024.0) * std::sin(4.0*phi);
    const double term4 = (35.0*e6/3072.0) * std::sin(6.0*phi);
    const double M = a * (term1 - term2 + term3 - term4);

    // Snyder (1987) eq. 8-13 to 8-15. 5th-order 系数:
    //   x: A + (1-T+C)/6 A³ + (5 - 18T + T² + 72C - 58e'²)/120 A⁵
    //   y: M + N tanφ * [A²/2 + (5-T+9C+4C²)/24 A⁴ + (61-58T+T²+600C-330e'²)/720 A⁶]
    x = k0 * N * (A + (1.0 - T + C) * A*A*A/6.0
                   + (5.0 - 18.0*T + T*T + 72.0*C - 58.0 * ep2) * A*A*A*A*A/120.0)
        + p.x_0;
    y = k0 * (M + N * tan_phi * (A*A/2.0
        + (5.0 - T + 9.0*C + 4.0*C*C) * A*A*A*A/24.0
        + (61.0 - 58.0*T + T*T + 600.0*C - 330.0 * ep2) * A*A*A*A*A*A/720.0))
        + p.y_0;
    return true;
}

// 高斯-克吕格反算 (XY→BL): 与正算对称
bool gkInverse(const ProjSpec& p, double x, double y, double& lat_deg, double& lon_deg) {
    const double e2 = p.ell.e2();
    const double e4 = p.ell.e4();
    const double e6 = p.ell.e6();
    const double a  = p.ell.a;
    const double k0 = p.k_0;
    const double lon0 = p.lon_0;

    x -= p.x_0;
    y -= p.y_0;
    const double M = y / k0;
    const double mu = M / (a * (1.0 - e2/4.0 - 3.0*e4/64.0 - 5.0*e6/256.0));

    const double e1 = (1.0 - std::sqrt(1.0 - e2)) / (1.0 + std::sqrt(1.0 - e2));
    const double mu2 = mu * mu;
    const double mu4 = mu2 * mu2;
    const double mu6 = mu4 * mu2;

    const double J1 = 3.0*e1/2.0 - 27.0*e1*e1*e1/32.0;
    const double J2 = 21.0*e1*e1/16.0 - 55.0*e1*e1*e1*e1/32.0;
    const double J3 = 151.0*e1*e1*e1/96.0;
    const double J4 = 1097.0*e1*e1*e1*e1/512.0;

    const double fp = mu + J1*std::sin(2.0*mu) + J2*std::sin(4.0*mu)
                     + J3*std::sin(6.0*mu) + J4*std::sin(8.0*mu);

    const double sin_fp = std::sin(fp);
    const double cos_fp = std::cos(fp);
    const double tan_fp = std::tan(fp);

    const double C1 = e2 * cos_fp * cos_fp / (1.0 - e2);
    const double T1 = tan_fp * tan_fp;
    const double ep2 = e2 / (1.0 - e2);
    const double R1 = a * (1.0 - e2) / std::pow(1.0 - e2*sin_fp*sin_fp, 1.5);
    const double N1 = a / std::sqrt(1.0 - e2*sin_fp*sin_fp);
    const double D  = x / (N1 * k0);

    const double Q1 = N1 * tan_fp / R1;
    const double Q2 = D*D/2.0;
    // Snyder (1987) eq. 8-19 to 8-22. 反算高阶项带 e'² (ep2) 修正.
    const double Q3 = (5.0 + 3.0*T1 + 10.0*C1 - 4.0*C1*C1 - 9.0*ep2) * D*D*D*D / 24.0;
    const double Q4 = (61.0 + 90.0*T1 + 298.0*C1 + 45.0*T1*T1 - 252.0*ep2 - 3.0*ep2) * D*D*D*D*D*D / 720.0;

    lat_deg = rad2deg(fp - Q1*(Q2 - Q3 + Q4));

    const double Q5 = D;
    const double Q6 = (1.0 + 2.0*T1 + C1) * D*D*D/6.0;
    const double Q7 = (5.0 - 2.0*C1 + 28.0*T1 - 3.0*C1*C1 + 8.0*ep2 + 24.0*T1*T1) * D*D*D*D*D/120.0;
    lon_deg = lon0 + rad2deg(Q5 - Q6 + Q7) / cos_fp;
    return true;
}

} // anonymous namespace

bool lonLatTo3857(double lon_deg, double lat_deg, double& mx, double& my) {
    return llTo3857Raw(lon_deg, lat_deg, mx, my);
}

bool merc3857ToLonLat(double mx, double my, double& lon_deg, double& lat_deg) {
    return wm3857ToLlRaw(mx, my, lon_deg, lat_deg);
}

std::optional<ProjSpec> projSpecFromEpsg(std::uint32_t epsg) {
    if (epsg == 4326) {
        ProjSpec p; p.kind = ProjKind::LatLon; p.ell = ellipsoid::WGS84(); return p;
    }
    if (epsg == 3857) {
        ProjSpec p; p.kind = ProjKind::WebMercator; p.ell = ellipsoid::WGS84(); return p;
    }
    // UTM 北半球
    if (epsg >= 32601 && epsg <= 32660) {
        ProjSpec p; p.kind = ProjKind::Utm; p.ell = ellipsoid::WGS84();
        p.lon_0 = (epsg - 32600) * 6.0 - 183.0; // zone 1 = -177, zone 60 = +177
        p.x_0 = 500000.0; p.k_0 = 0.9996; p.south = false;
        return p;
    }
    // UTM 南半球
    if (epsg >= 32701 && epsg <= 32760) {
        ProjSpec p; p.kind = ProjKind::Utm; p.ell = ellipsoid::WGS84();
        p.lon_0 = (epsg - 32700) * 6.0 - 183.0;
        p.x_0 = 500000.0; p.y_0 = 10000000.0; p.k_0 = 0.9996; p.south = true;
        return p;
    }
    // CGCS2000 6°GK CM-named: 4502..4512 (CM 75°E..135°E)
    if (epsg >= 4502 && epsg <= 4512) {
        ProjSpec p; p.kind = ProjKind::GaussKrCm; p.ell = ellipsoid::GRS80();
        const std::uint32_t idx = epsg - 4502; // 0=75E, ..., 10=135E
        p.lon_0 = 75.0 + idx * 6.0;
        p.x_0 = 500000.0; p.k_0 = 1.0;
        return p;
    }
    // CGCS2000 3°GK zone 25..45: 4513..4533
    if (epsg >= 4513 && epsg <= 4533) {
        ProjSpec p; p.kind = ProjKind::GaussKrZone; p.ell = ellipsoid::GRS80();
        const std::uint32_t zone = epsg - 4488; // 4513→25, 4533→45
        p.lon_0 = zone * 3.0;
        p.x_0 = (zone + 0.5) * 1000000.0; p.k_0 = 1.0;
        return p;
    }
    // 西安80 6°GK zone 13..23: 2327..2337
    if (epsg >= 2327 && epsg <= 2337) {
        ProjSpec p; p.kind = ProjKind::GaussKrZone; p.ell = ellipsoid::IAU76();
        const std::uint32_t zone = epsg - 2314; // 2327→13, 2337→23
        p.lon_0 = (zone - 13.0) * 6.0 + 75.0;
        p.x_0 = (zone + 0.5) * 1000000.0; p.k_0 = 1.0;
        return p;
    }
    // 西安80 6°GK CM: 2338..2342
    if (epsg >= 2338 && epsg <= 2342) {
        ProjSpec p; p.kind = ProjKind::GaussKrCm; p.ell = ellipsoid::IAU76();
        const std::uint32_t idx = epsg - 2338;
        p.lon_0 = 75.0 + idx * 6.0;
        p.x_0 = 500000.0; p.k_0 = 1.0;
        return p;
    }
    // 西安80 3°GK zone 38..45: 2362..2369
    if (epsg >= 2362 && epsg <= 2369) {
        ProjSpec p; p.kind = ProjKind::GaussKrZone; p.ell = ellipsoid::IAU76();
        const std::uint32_t zone = epsg - 2324; // 2362→38
        p.lon_0 = zone * 3.0;
        p.x_0 = (zone + 0.5) * 1000000.0; p.k_0 = 1.0;
        return p;
    }
    // 西安80 3°GK CM 75..117E: 2370..2384
    if (epsg >= 2370 && epsg <= 2384) {
        ProjSpec p; p.kind = ProjKind::GaussKrCm; p.ell = ellipsoid::IAU76();
        const std::uint32_t idx = epsg - 2370;
        p.lon_0 = 75.0 + idx * 3.0;
        p.x_0 = 500000.0; p.k_0 = 1.0;
        return p;
    }
    // 北京54 3°GK zone 25..45: 2401..2421
    if (epsg >= 2401 && epsg <= 2421) {
        ProjSpec p; p.kind = ProjKind::GaussKrZone; p.ell = ellipsoid::Kras1940();
        const std::uint32_t zone = epsg - 2376; // 2401→25
        p.lon_0 = zone * 3.0;
        p.x_0 = (zone + 0.5) * 1000000.0; p.k_0 = 1.0;
        return p;
    }
    // 北京54 3°GK CM 75..90E: 2422..2427
    if (epsg >= 2422 && epsg <= 2427) {
        ProjSpec p; p.kind = ProjKind::GaussKrCm; p.ell = ellipsoid::Kras1940();
        const std::uint32_t idx = epsg - 2422;
        p.lon_0 = 75.0 + idx * 3.0;
        p.x_0 = 500000.0; p.k_0 = 1.0;
        return p;
    }
    return std::nullopt;
}

bool transformTo3857(std::uint32_t src_epsg, double x, double y, double& mx, double& my) {
    auto spec = projSpecFromEpsg(src_epsg);
    if (!spec) return false;
    switch (spec->kind) {
    case ProjKind::WebMercator:
        mx = x; my = y; return true;
    case ProjKind::LatLon:
        return llTo3857Raw(x, y, mx, my);
    case ProjKind::Utm:
    case ProjKind::GaussKrZone:
    case ProjKind::GaussKrCm: {
        double lon, lat;
        gkInverse(*spec, x, y, lat, lon);
        return llTo3857Raw(lon, lat, mx, my);
    }
    default:
        return false;
    }
}

bool transform3857ToSrc(std::uint32_t src_epsg, double mx, double my, double& x, double& y) {
    auto spec = projSpecFromEpsg(src_epsg);
    if (!spec) return false;
    switch (spec->kind) {
    case ProjKind::WebMercator:
        x = mx; y = my; return true;
    case ProjKind::LatLon:
        return wm3857ToLlRaw(mx, my, x, y);
    case ProjKind::Utm:
    case ProjKind::GaussKrZone:
    case ProjKind::GaussKrCm: {
        double lon, lat;
        wm3857ToLlRaw(mx, my, lon, lat);
        return gkForward(*spec, lat, lon, x, y);
    }
    default:
        return false;
    }
}

} // namespace qcutter