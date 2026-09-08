#include "engine/meta.h"
#include "engine/error.h"
#include "engine/proj_engine.h"
#include "engine/source.h"
#include <tiffio.h>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace qcutter {

namespace {
constexpr std::uint16_t TAG_MODEL_PIXEL_SCALE   = 33550;
constexpr std::uint16_t TAG_MODEL_TIEPOINT      = 33922;
constexpr std::uint16_t TAG_GEO_KEY_DIRECTORY   = 34735;
constexpr std::uint16_t TAG_GEO_ASCII_PARAMS    = 34737;
constexpr std::uint16_t TAG_GDAL_NODATA         = 42113;
constexpr std::uint16_t GT_MODEL_TYPE_GEOKEY    = 1024;
constexpr std::uint16_t PROJECTED_CS_TYPE_GEOKEY = 3072;
constexpr std::uint16_t GEOGRAPHIC_TYPE_GEOKEY  = 2049;
constexpr std::uint32_t MODEL_TYPE_PROJECTED    = 1;
constexpr std::uint32_t MODEL_TYPE_GEOGRAPHIC   = 2;
constexpr double ORIGIN_SHIFT                    = 20037508.342789244;

// libtiff 默认不识别 GeoTIFF tags (33550/33922/34735/34737/42113).
// 用 TIFFSetTagExtender() 全局注册这些 FIELD_CUSTOM tags,
// 这样 TIFFReadDirectory 会把它们的值存入 td_customValues,
// 后续 TIFFGetField(tif, tag, &count, &data) 能正确返回.
// TIFF_VARIABLE = -1, FIELD_CUSTOM = 6.
// 注意: TIFFFieldInfo 公共结构只有 8 个字段 (没有子字段数组).
static void georefTagExtender(TIFF* tif) {
    static const TIFFFieldInfo georefFields[] = {
        { TAG_MODEL_PIXEL_SCALE, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_DOUBLE,
          FIELD_CUSTOM, 1, 1, "ModelPixelScale" },
        { TAG_MODEL_TIEPOINT,    TIFF_VARIABLE, TIFF_VARIABLE, TIFF_DOUBLE,
          FIELD_CUSTOM, 1, 1, "ModelTiepoint" },
        { TAG_GEO_KEY_DIRECTORY, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SHORT,
          FIELD_CUSTOM, 1, 1, "GeoKeyDirectory" },
        { TAG_GEO_ASCII_PARAMS,  TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII,
          FIELD_CUSTOM, 1, 1, "GeoAsciiParams" },
        { TAG_GDAL_NODATA,       TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII,
          FIELD_CUSTOM, 1, 1, "GDAL_NODATA" },
    };
    TIFFMergeFieldInfo(tif, georefFields,
                       sizeof(georefFields) / sizeof(georefFields[0]));
}

// 读 pass-count FIELD_CUSTOM tag: TIFFGetField(tif, tag, &count, &data) 两参一次.
// libtiff 把 count 写到 &count, 把内部 tv->value 指针写到 &data.
// 拿到 const T* 指针后直接 assign 给 std::vector.
bool getTagF64Vec(TIFF* tif, std::uint16_t tag, std::vector<double>& out) {
    std::uint16_t count = 0;
    const double* data = nullptr;
    if (TIFFGetField(tif, tag, &count, &data) && count > 0 && data != nullptr) {
        out.assign(data, data + count);
        return true;
    }
    out.clear();
    return false;
}

bool getTagU32Vec(TIFF* tif, std::uint16_t tag, std::vector<std::uint32_t>& out) {
    std::uint16_t count = 0;
    const std::uint32_t* data = nullptr;
    if (TIFFGetField(tif, tag, &count, &data) && count > 0 && data != nullptr) {
        out.assign(data, data + count);
        return true;
    }
    out.clear();
    return false;
}

bool getTagU16Vec(TIFF* tif, std::uint16_t tag, std::vector<std::uint16_t>& out) {
    std::uint16_t count = 0;
    const std::uint16_t* data = nullptr;
    if (TIFFGetField(tif, tag, &count, &data) && count > 0 && data != nullptr) {
        out.assign(data, data + count);
        return true;
    }
    out.clear();
    return false;
}
} // anonymous

// installGeorefTags 必须在 qcutter namespace 顶层, 与 meta.h 中
// void qcutter::installGeorefTags() 声明匹配. 内部使用匿名 namespace 中的
// georefTagExtender 通过内部链接.
void installGeorefTags() {
    static bool installed = false;
    if (!installed) {
        TIFFSetTagExtender(georefTagExtender);
        installed = true;
    }
}

std::optional<std::uint32_t> parseGeoKeyEpsg(const std::vector<std::uint32_t>& keys) {
    if (keys.size() < 4) return std::nullopt;
    const std::uint32_t n_keys = keys[3];
    std::uint32_t model_type = 0;
    std::uint32_t projected_epsg = 0;
    std::uint32_t geographic_epsg = 0;

    for (std::uint32_t i = 0; i < n_keys; ++i) {
        const std::size_t base = 4 + static_cast<std::size_t>(i) * 4;
        if (base + 4 > keys.size()) break;
        const std::uint16_t key_id = static_cast<std::uint16_t>(keys[base]);
        const std::uint32_t value = keys[base + 3];
        switch (key_id) {
        case GT_MODEL_TYPE_GEOKEY:     model_type = value; break;
        case PROJECTED_CS_TYPE_GEOKEY: projected_epsg = value; break;
        case GEOGRAPHIC_TYPE_GEOKEY:   geographic_epsg = value; break;
        }
    }
    if (model_type == MODEL_TYPE_PROJECTED && projected_epsg > 0 && projected_epsg < 32767)
        return projected_epsg;
    if (model_type == MODEL_TYPE_GEOGRAPHIC && geographic_epsg > 0 && geographic_epsg < 32767)
        return geographic_epsg;
    return std::nullopt;
}

std::optional<GeoRef> probeGeoref(const fs::path& path) {
    installGeorefTags();  // 全局一次,确保 GeoTIFF tags 已被 libtiff 识别
    TIFF* tif = TIFFOpen(path.string().c_str(), "r");
    if (!tif) {
        throw CoreError::io(path.string(), "TIFFOpen failed");
    }
    struct Closer { TIFF* t; ~Closer() { if (t) TIFFClose(t); } } cl{tif};

    std::vector<double> scale, tie;
    if (!getTagF64Vec(tif, TAG_MODEL_PIXEL_SCALE, scale)
        || !getTagF64Vec(tif, TAG_MODEL_TIEPOINT, tie)) {
        return std::nullopt;
    }
    if (scale.size() < 2 || tie.size() < 6 || scale[0] <= 0.0 || scale[1] == 0.0) {
        return std::nullopt;
    }
    const double raw_x = tie[3];
    const double raw_y = tie[4];
    const double px = scale[0];
    const double py = scale[1];

    // 尝试 GeoKeyDirectory (u16 或 u32)
    std::optional<std::uint32_t> epsg_opt;
    {
        std::vector<std::uint16_t> v16;
        std::vector<std::uint32_t> v32;
        if (getTagU16Vec(tif, TAG_GEO_KEY_DIRECTORY, v16)) {
            std::vector<std::uint32_t> conv(v16.size());
            std::transform(v16.begin(), v16.end(), conv.begin(),
                           [](std::uint16_t x) { return static_cast<std::uint32_t>(x); });
            epsg_opt = parseGeoKeyEpsg(conv);
        } else if (getTagU32Vec(tif, TAG_GEO_KEY_DIRECTORY, v32)) {
            epsg_opt = parseGeoKeyEpsg(v32);
        }
    }

    std::uint32_t src_w = 1, src_h = 1;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &src_w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &src_h);

    const bool is_3857 = (epsg_opt && *epsg_opt == 3857);
    const bool is_4326 = (epsg_opt && *epsg_opt == 4326);

    const bool heuristic_3857 =
        !epsg_opt.has_value()
        && std::abs(raw_x) <= ORIGIN_SHIFT && std::abs(raw_y) <= ORIGIN_SHIFT
        && std::abs(raw_x) > 360.0;
    const bool heuristic_4326 =
        !epsg_opt.has_value()
        && std::abs(raw_x) <= 360.0 && std::abs(raw_y) <= 90.0 && px < 1.0;

    (void)src_w;
    (void)src_h;

    // 任何已知投影都通过 transformTo3857 自动转为 3857 米坐标,
    // 不再仅支持 3857/4326. 这样 CGCS2000 / UTM / 高斯-克吕格 等中国常用 EPSG 都能切片.
    if (epsg_opt.has_value()) {
        const std::uint32_t src_epsg = *epsg_opt;
        double mx = 0.0, my = 0.0;
        if (transformTo3857(src_epsg, raw_x, raw_y, mx, my)) {
            // 像素尺寸换算 (源投影 → EPSG:3857 米).
            // 关键: 不能用源投影自身的米/像素直接当 3857 米/像素.
            // 应当用四角点投影后做差, 以正确反映 3857 球面度量的真实尺度.
            //
            // 例: UTM48N 上 0.5 m/px (真实 0.5 米) 在 3857 上是 ~0.552 m/px
            // (cosLat 因子), 在 Z18 直接造成 84 个 columns vs 77 个 columns 的差异.
            //
            // 对 EPSG:4326 (经纬度): px/py 是度, 直接换算为 3857 米.
            // 对 UTM / GK 等米投影: 用 TR (raw_x + w*px, raw_y) 与 BL (raw_x, raw_y - h*py)
            // 投影到 3857 后的差分.
            // 对 EPSG:3857 (源已是 Web Mercator): 直接用 px/py.
            double sx_m = std::abs(px);
            double sy_m = std::abs(py);
            if (src_epsg == 4326) {
                // 取 TL 经纬度.
                double cLat = raw_y;
                const double cosLat = std::cos(cLat * M_PI / 180.0);
                // px/py 是 (经度/像素) 与 (纬度/像素) 度, 转 3857 米.
                sx_m = std::abs(px) * 111320.0 * cosLat;
                sy_m = std::abs(py) * 111320.0;
            } else if (src_epsg != 3857) {
                // UTM / GK 等米投影: 计算角点投影后的差分.
                // TR: (raw_x + src_w * px, raw_y)
                // BL: (raw_x, raw_y - src_h * py)
                // sx_m = (TR.x - TL.x) / src_w
                // sy_m = (TL.y - BL.y) / src_h (取正值; GeoRef.sy 之后会取负)
                double mx_tr = 0.0, my_unused1 = 0.0;
                double mx_unused2 = 0.0, my_bl = 0.0;
                std::uint32_t src_w = 1, src_h = 1;
                TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &src_w);
                TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &src_h);
                const bool tr_ok = transformTo3857(src_epsg,
                                                   raw_x + static_cast<double>(src_w) * px,
                                                   raw_y,
                                                   mx_tr, my_unused1);
                const bool bl_ok = transformTo3857(src_epsg,
                                                   raw_x,
                                                   raw_y - static_cast<double>(src_h) * py,
                                                   mx_unused2, my_bl);
                if (tr_ok && bl_ok && src_w > 0 && src_h > 0) {
                    sx_m = std::abs(mx_tr - mx) / static_cast<double>(src_w);
                    sy_m = std::abs(my - my_bl) / static_cast<double>(src_h);
                }
                // 任一角点投影失败则回退到原启发式 (cosLat).
                // 此时仍优于不修正, 但仅作防御。
            }
            return GeoRef{ mx, my, sx_m, -sy_m, src_epsg,
                           raw_x, raw_y, px, py };
        }
        // transformTo3857 不支持的 EPSG: 回落到下面的启发式判定
    }

    if (is_3857 || heuristic_3857) {
        return GeoRef{ raw_x, raw_y,  px, -py,
                       epsg_opt.value_or(3857),
                       raw_x, raw_y, px, py };
    }
    if (is_4326 || heuristic_4326) {
        return GeoRef{ raw_x, raw_y,  px, -py,
                       epsg_opt.value_or(4326),
                       raw_x, raw_y, px, py };
    }
    return std::nullopt;
}

} // namespace qcutter
