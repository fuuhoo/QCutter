#include "engine/cutter.h"
#include "engine/alpha.h"
#include "engine/error.h"
#include "engine/mercator.h"
#include "engine/meta.h"

// stb_image_write: 单头 PNG 编码, 多线程安全, 无外部依赖.
// QImage::save 在 worker 线程并发会 SIGSEGV in memcpy.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "3rdparty/stb_image_write.h"
#include "engine/planner.h"
#include "engine/proj_engine.h"
#include "engine/source.h"
#include "engine/writer.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QIODevice>
#include <QThread>
#include <QtCore/QtMath>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <cstring>
#include "util/fs_compat.h"
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

namespace qcutter {

// ------------------ 预乘 alpha 工具 ------------------

static void premultiplyInplace(std::uint8_t* rgba, std::size_t n_px) {
    for (std::size_t i = 0; i < n_px; ++i) {
        std::uint8_t* p = rgba + i*4;
        if (p[3] == 255) continue;
        const unsigned a = p[3];
        p[0] = static_cast<std::uint8_t>((unsigned)p[0] * a / 255);
        p[1] = static_cast<std::uint8_t>((unsigned)p[1] * a / 255);
        p[2] = static_cast<std::uint8_t>((unsigned)p[2] * a / 255);
    }
}

static void unpremultiplyInplace(std::uint8_t* rgba, std::size_t n_px) {
    for (std::size_t i = 0; i < n_px; ++i) {
        std::uint8_t* p = rgba + i*4;
        const unsigned a = p[3];
        if (a == 0) { p[0]=p[1]=p[2]=0; continue; }
        if (a == 255) continue;
        p[0] = static_cast<std::uint8_t>(std::min<unsigned>(255u, (unsigned)p[0]*255u/a));
        p[1] = static_cast<std::uint8_t>(std::min<unsigned>(255u, (unsigned)p[1]*255u/a));
        p[2] = static_cast<std::uint8_t>(std::min<unsigned>(255u, (unsigned)p[2]*255u/a));
    }
}

// ------------------ 预乘感知缩放 (替代 image::imageops::resize) ------------------

/// 简单双线性: 输入 RGBA, 输出 RGBA, 预乘空间. 输入与输出宽高不同则做插值.
/// 这是简化的、与 Lanczos3 接近的二次插值; 我们用其替代 image crate 的 Lanczos3.
static void bilinearResizePremul(const std::uint8_t* in, int iw, int ih,
                                  std::uint8_t* out, int ow, int oh) {
    if (iw == ow && ih == oh) {
        std::memcpy(out, in, static_cast<std::size_t>(iw)*ih*4);
        return;
    }
    auto fetchPremul = [&](int x, int y, int& xc, int& yc) {
        x = std::clamp(x, 0, iw - 1);
        y = std::clamp(y, 0, ih - 1);
        xc = x; yc = y;
        const std::size_t off = (static_cast<std::size_t>(y)*iw + x)*4;
        return in + off;
    };
    for (int y = 0; y < oh; ++y) {
        const double fy = (ih > oh) ? (double)y * (ih - 1) / (oh - 1) : (double)y;
        const int yi0 = static_cast<int>(fy);
        const int yi1 = std::min(yi0 + 1, ih - 1);
        const double wy = fy - yi0;
        for (int x = 0; x < ow; ++x) {
            const double fx = (iw > ow) ? (double)x * (iw - 1) / (ow - 1) : (double)x;
            const int xi0 = static_cast<int>(fx);
            const int xi1 = std::min(xi0 + 1, iw - 1);
            const double wx = fx - xi0;
            // 四个角 (a=预乘) 加权
            int xa, ya;
            const std::uint8_t* p00 = fetchPremul(xi0, yi0, xa, ya);
            const std::uint8_t* p01 = fetchPremul(xi0, yi1, xa, ya);
            const std::uint8_t* p10 = fetchPremul(xi1, yi0, xa, ya);
            const std::uint8_t* p11 = fetchPremul(xi1, yi1, xa, ya);
            const double w00 = (1.0 - wx) * (1.0 - wy);
            const double w01 = (1.0 - wx) * wy;
            const double w10 = wx * (1.0 - wy);
            const double w11 = wx * wy;
            std::uint8_t* dst = out + (static_cast<std::size_t>(y)*ow + x)*4;
            for (int c = 0; c < 4; ++c) {
                const double v = p00[c]*w00 + p01[c]*w01 + p10[c]*w10 + p11[c]*w11;
                dst[c] = static_cast<std::uint8_t>(std::min(255.0, std::max(0.0, v)));
            }
        }
    }
}

static void resizePremul(const std::uint8_t* in, int iw, int ih,
                         std::uint8_t* out, int ow, int oh) {
    bilinearResizePremul(in, iw, ih, out, ow, oh);
}

// ------------------ PNG 编码 (stb_image_write + 全局 mutex) ------------------
//
// stb_image_write 内部 PNG/zlib 状态非线程安全 (stbi_zlib_compress 用 macros 共享
// bitbuf/bitcount). 多 worker 线程并发 SIGSEGV in memcpy. 全局 mutex 串行化 PNG 编码.
// PNG 编码是轻量级, 主要时间在 TIFF 解码 (TIFFReadEncodedTile ~几十 ms/tile,
// PNG 编码 < 1 ms/tile), 串行化影响很小.
static std::mutex g_png_encode_mu;
static std::vector<std::uint8_t> encodePng(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> out;
    auto write_cb = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
        auto* p = static_cast<std::uint8_t*>(data);
        v->insert(v->end(), p, p + size);
    };
    std::lock_guard<std::mutex> lk(g_png_encode_mu);
    stbi_write_png_to_func(write_cb, &out,
                           static_cast<int>(w), static_cast<int>(h),
                           4,  // RGBA
                           const_cast<std::uint8_t*>(rgba),
                           static_cast<int>(w) * 4);
    return out;
}

// ------------------ 瓦片写入 ------------------

static std::uint64_t writeTile(const CutParams& params,
                                std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                                const fs::path& rel) {
    params.alpha.apply(rgba, static_cast<std::size_t>(w) * h);
    if (params.skip_empty) {
        bool any_opaque = false;
        for (std::size_t i = 3; i < static_cast<std::size_t>(w) * h * 4; i += 4) {
            if (rgba[i] != 0) { any_opaque = true; break; }
        }
        if (!any_opaque) return 0;
    }
    auto png = encodePng(rgba, w, h);
    const auto p = params.output / rel;
    std::error_code ec2;
    fs::create_directories(p.parent_path(), ec2);
    std::ofstream f(p.string(), std::ios::binary);
    if (!f) throw CoreError::io(p.string(), "open for write failed");
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return png.size();
}

// ------------------ 全图 mip 金字塔 (共享, 巨型条带源) ------------------

struct FullMips {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> dims;
    std::vector<std::shared_ptr<std::vector<std::uint8_t>>> lv; // 每级 RGBA
};

/// 2x 盒式平均 (预乘感知).
static void halve2x(const std::vector<std::uint8_t>& src, std::uint32_t w, std::uint32_t h,
                    std::vector<std::uint8_t>& dst, std::uint32_t& nw, std::uint32_t& nh) {
    nw = std::max<std::uint32_t>(1, (w + 1) / 2);
    nh = std::max<std::uint32_t>(1, (h + 1) / 2);
    dst.assign(static_cast<std::size_t>(nw) * nh * 4, 0);
    for (std::uint32_t oy = 0; oy < nh; ++oy) {
        for (std::uint32_t ox = 0; ox < nw; ++ox) {
            unsigned sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
            for (unsigned dy = 0; dy < 2; ++dy) {
                for (unsigned dx = 0; dx < 2; ++dx) {
                    const std::uint32_t x = ox*2 + dx;
                    const std::uint32_t y = oy*2 + dy;
                    if (x < w && y < h) {
                        const std::size_t i = (static_cast<std::size_t>(y)*w + x)*4;
                        const unsigned a = src[i+3];
                        sr += src[i] * a;
                        sg += src[i+1] * a;
                        sb += src[i+2] * a;
                        sa += a;
                        cnt += 1;
                    }
                }
            }
            const std::size_t di = (static_cast<std::size_t>(oy)*nw + ox)*4;
            if (sa > 0) {
                dst[di+0] = static_cast<std::uint8_t>(sr / sa);
                dst[di+1] = static_cast<std::uint8_t>(sg / sa);
                dst[di+2] = static_cast<std::uint8_t>(sb / sa);
            }
            dst[di+3] = static_cast<std::uint8_t>(sa / std::max<unsigned>(1, cnt));
        }
    }
}

/// 多线程版 halve2x: 按行分块, 每 worker 写独立行段到 dst 不同偏移. 大图上
/// 单次 halve2x 是 O(n_px) ≈ 数亿像素 → 数秒, 多线程接近线性加速.
static void halve2xParallel(const std::vector<std::uint8_t>& src, std::uint32_t w, std::uint32_t h,
                            std::vector<std::uint8_t>& dst, std::uint32_t& nw, std::uint32_t& nh) {
    nw = std::max<std::uint32_t>(1, (w + 1) / 2);
    nh = std::max<std::uint32_t>(1, (h + 1) / 2);
    dst.assign(static_cast<std::size_t>(nw) * nh * 4, 0);
    const unsigned hw = std::max<unsigned>(1, std::thread::hardware_concurrency());
    if (nh < hw * 4 || hw == 1) {
        // 行数太少, 单线程更快 (避免 thread 启动开销)
        halve2x(src, w, h, dst, nw, nh);
        return;
    }
    // 每 worker 处理 [y0, y1) 行. dst 按行主序布局, 无重叠, 不需要锁.
    std::vector<std::future<void>> futs;
    const std::uint32_t chunk = (nh + hw - 1) / hw;
    for (unsigned t = 0; t < hw; ++t) {
        const std::uint32_t y0 = t * chunk;
        if (y0 >= nh) break;
        const std::uint32_t y1 = std::min<std::uint32_t>(y0 + chunk, nh);
        futs.push_back(std::async(std::launch::async, [&src, w, h, nw, &dst, y0, y1]() {
            for (std::uint32_t oy = y0; oy < y1; ++oy) {
                for (std::uint32_t ox = 0; ox < nw; ++ox) {
                    unsigned sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
                    for (unsigned dy = 0; dy < 2; ++dy) {
                        for (unsigned dx = 0; dx < 2; ++dx) {
                            const std::uint32_t x = ox*2 + dx;
                            const std::uint32_t y = oy*2 + dy;
                            if (x < w && y < h) {
                                const std::size_t i = (static_cast<std::size_t>(y)*w + x)*4;
                                const unsigned a = src[i+3];
                                sr += src[i] * a;
                                sg += src[i+1] * a;
                                sb += src[i+2] * a;
                                sa += a;
                                cnt += 1;
                            }
                        }
                    }
                    const std::size_t di = (static_cast<std::size_t>(oy)*nw + ox)*4;
                    if (sa > 0) {
                        dst[di+0] = static_cast<std::uint8_t>(sr / sa);
                        dst[di+1] = static_cast<std::uint8_t>(sg / sa);
                        dst[di+2] = static_cast<std::uint8_t>(sb / sa);
                    }
                    dst[di+3] = static_cast<std::uint8_t>(sa / std::max<unsigned>(1, cnt));
                }
            }
        }));
    }
    for (auto& f : futs) f.wait();
}

/// 多线程构造 mip 金字塔. 各级之间有数据依赖, 严格串行; 但单次 halve2x 内部
/// 按行分块并行, 大图(>4MB 像素) 上单次 halve2x 耗时显著, 多线程能拿到
/// 接近 hw 倍加速. 由并行版 halve2xRows 承担, 串行版 halve2x 仅作小图 fallback.
static FullMips buildMips(QImage img) {
    FullMips fm;
    fm.dims.push_back({ static_cast<std::uint32_t>(img.width()),
                        static_cast<std::uint32_t>(img.height()) });
    auto raw = std::make_shared<std::vector<std::uint8_t>>(img.bits(), img.bits() + img.sizeInBytes());
    fm.lv.push_back(raw);
    std::uint32_t w = static_cast<std::uint32_t>(img.width());
    std::uint32_t h = static_cast<std::uint32_t>(img.height());
    const auto base_bytes = static_cast<std::uint64_t>(w) * h * 4ULL;
    std::uint64_t extra = 0;
    const auto pix_count = static_cast<std::uint64_t>(w) * h;
    // 小图 (像素数 < 1M) 走单线程, 避免线程开销
    const bool use_parallel = pix_count >= (1ULL << 20);
    while (std::max(w, h) > 512 && fm.lv.size() < 14) {
        std::vector<std::uint8_t> next;
        std::uint32_t nw, nh;
        if (use_parallel) {
            halve2xParallel(*fm.lv.back(), w, h, next, nw, nh);
        } else {
            halve2x(*fm.lv.back(), w, h, next, nw, nh);
        }
        extra += static_cast<std::uint64_t>(nw) * nh * 4ULL;
        if (extra > base_bytes / 2) break;
        fm.dims.push_back({ nw, nh });
        fm.lv.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(next)));
        w = nw; h = nh;
    }
    return fm;
}

// ------------------ 切片核心 ------------------

namespace {

struct Job {
    std::uint32_t z;
    std::uint32_t tx;
    std::uint32_t ty;
    std::uint32_t tiles_y;
    std::uint32_t plan_idx;
};

inline std::uint32_t pickK(std::int64_t span_full, std::int64_t out_px, std::size_t n_levels) {
    std::uint32_t k = 0;
    while (k + 1 < n_levels) {
        const std::int64_t next_span = span_full >> (k + 1);
        if (next_span >= std::max<std::int64_t>(out_px, 1)) k += 1;
        else break;
    }
    return k;
}

struct RunCtx {
    std::vector<Job> jobs;
    std::optional<PyramidPlan> pyramid;
    std::optional<MercPlan> mplan;
    std::optional<GeoRef> geo;
    std::array<double, 4> merc_bounds{};
    std::uint32_t base_z = 0;
};

/// Mercator 模式 (无 mip): 线性反算 + 读源矩形 + 重采样.
static std::vector<std::uint8_t> renderTileMercator(SourceReader& reader,
                                                   const CutParams& params,
                                                   const GeoRef& g,
                                                   std::uint32_t z, std::uint32_t tx, std::uint32_t ty) {
    const double t = params.tile_size;
    const double res = MERC_INIT_RESOLUTION / std::pow(2.0, static_cast<int>(z));
    const double wx_l = tx * t * res - MERC_ORIGIN_SHIFT;
    const double wx_r = (tx + 1) * t * res - MERC_ORIGIN_SHIFT;
    const double wy_top = MERC_ORIGIN_SHIFT - ty * t * res;
    const double wy_bot = MERC_ORIGIN_SHIFT - (ty + 1) * t * res;
    const double fx0 = (wx_l - g.mx0) / g.sx;
    const double fx1 = (wx_r - g.mx0) / g.sx;
    const double fy_top = (wy_top - g.my_top) / g.sy;
    const double fy_bot = (wy_bot - g.my_top) / g.sy;
    const std::int64_t rx = static_cast<std::int64_t>(std::floor(std::min(fx0, fx1)));
    const std::uint32_t rw = static_cast<std::uint32_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(std::max(fx0, fx1))) - rx));
    const std::int64_t ry = static_cast<std::int64_t>(std::floor(std::min(fy_top, fy_bot)));
    const std::uint32_t rh = static_cast<std::uint32_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(std::max(fy_top, fy_bot))) - ry));
    auto crop = reader.readRect(rx, ry, rw, rh);
    QImage img(reinterpret_cast<const std::uint8_t*>(crop.data()),
               static_cast<int>(rw), static_cast<int>(rh), static_cast<int>(rw)*4,
               QImage::Format_RGBA8888);
    if (rw > 0 && rh > 0) {
        img = img.copy(); // 重新分配内存, 解开 readRect 临时缓冲
    }
    QImage out = img.convertToFormat(QImage::Format_RGBA8888).scaled(
        static_cast<int>(params.tile_size), static_cast<int>(params.tile_size),
        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // scaled 输出实际为 Format_ARGB32_Premultiplied (Qt 5.12 缩放后变 premul),
    // 转回 RGBA8888 保证 [R G B A] 字节序, 喂给 stbi_write_png 才不会偏蓝.
    if (out.format() != QImage::Format_RGBA8888) {
        out = out.convertToFormat(QImage::Format_RGBA8888);
    }
    return std::vector<std::uint8_t>(out.bits(), out.bits() + out.sizeInBytes());
}

/// Mercator 模式 (精确反算 + proj_engine).
static std::vector<std::uint8_t> renderTileMercatorPrecise(SourceReader& reader,
                                                            const CutParams& params,
                                                            const GeoRef& g,
                                                            std::uint32_t z, std::uint32_t tx, std::uint32_t ty) {
    const std::uint32_t ts = params.tile_size;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(ts) * ts * 4, 0);
    const double res = MERC_INIT_RESOLUTION / std::pow(2.0, static_cast<int>(z));
    // 对每行: 算 256 个 wx 反算到 src utm, 然后转 src 像素 i,j.
    for (std::uint32_t j_dst = 0; j_dst < ts; ++j_dst) {
        const double wy = MERC_ORIGIN_SHIFT - (ty + (static_cast<double>(j_dst) + 0.5) / ts) * ts * res;
        std::vector<std::int64_t> i_arr(ts);
        std::vector<std::int64_t> j_arr(ts);
        std::vector<bool> in_bounds(ts);
        std::int64_t i_min = std::numeric_limits<std::int64_t>::max();
        std::int64_t i_max = std::numeric_limits<std::int64_t>::min();
        std::int64_t j_min = std::numeric_limits<std::int64_t>::max();
        std::int64_t j_max = std::numeric_limits<std::int64_t>::min();
        for (std::uint32_t i_dst = 0; i_dst < ts; ++i_dst) {
            const double wx = (tx + (static_cast<double>(i_dst) + 0.5) / ts) * ts * res - MERC_ORIGIN_SHIFT;
            double ux = 0, uy = 0;
            if (!transform3857ToSrc(g.src_epsg, wx, wy, ux, uy)) {
                in_bounds[i_dst] = false;
                continue;
            }
            const double i_f = (ux - g.src_tie_x) / g.src_px_w;
            const double j_f = (g.src_tie_y - uy) / g.src_px_h;
            const std::int64_t i_i = static_cast<std::int64_t>(std::round(i_f + 0.5));
            const std::int64_t j_i = static_cast<std::int64_t>(std::round(j_f + 0.5));
            i_arr[i_dst] = i_i;
            j_arr[i_dst] = j_i;
            const std::int64_t W = reader.width();
            const std::int64_t H = reader.height();
            const bool ok = (i_i >= 0 && i_i < W && j_i >= 0 && j_i < H);
            in_bounds[i_dst] = ok;
            if (ok) {
                i_min = std::min(i_min, i_i);
                i_max = std::max(i_max, i_i);
                j_min = std::min(j_min, j_i);
                j_max = std::max(j_max, j_i);
            }
        }
        if (i_min > i_max || j_min > j_max) continue;
        const std::uint32_t rw = static_cast<std::uint32_t>(i_max - i_min + 1);
        const std::uint32_t rh = static_cast<std::uint32_t>(j_max - j_min + 1);
        const std::int64_t W = reader.width();
        const std::int64_t H = reader.height();
        if (i_min < 0) i_min = 0;
        if (j_min < 0) j_min = 0;
        if (static_cast<std::uint64_t>(i_min) + rw > static_cast<std::uint64_t>(W)) {
            // clamp
        }
        const std::uint32_t rw_eff = static_cast<std::uint32_t>(std::min<std::int64_t>(i_min + rw, W) - i_min);
        const std::uint32_t rh_eff = static_cast<std::uint32_t>(std::min<std::int64_t>(j_min + rh, H) - j_min);
        if (rw_eff == 0 || rh_eff == 0) continue;
        auto rect = reader.readRect(i_min, j_min, rw_eff, rh_eff);
        for (std::uint32_t i_dst = 0; i_dst < ts; ++i_dst) {
            if (!in_bounds[i_dst]) continue;
            const std::int64_t ii = i_arr[i_dst] - i_min;
            const std::int64_t jj = j_arr[i_dst] - j_min;
            if (ii < 0 || jj < 0 || static_cast<std::uint32_t>(ii) >= rw_eff
                || static_cast<std::uint32_t>(jj) >= rh_eff) continue;
            const std::size_t src_off = (static_cast<std::size_t>(jj)*rw_eff + ii)*4;
            const std::size_t dst_off = (static_cast<std::size_t>(j_dst)*ts + i_dst)*4;
            std::memcpy(out.data() + dst_off, rect.data() + src_off, 4);
        }
    }
    return out;
}

/// 相对模式单瓦片.
static std::vector<std::uint8_t> renderTile(SourceReader& reader,
                                             const CutParams& params,
                                             const LevelPlan& lp,
                                             std::uint32_t tx, std::uint32_t ty) {
    const std::uint32_t t = params.tile_size;
    const std::uint32_t out_w = std::min<std::uint32_t>(t, lp.width - tx * t);
    const std::uint32_t out_h = std::min<std::uint32_t>(t, lp.height - ty * t);
    if (out_w == 0 || out_h == 0) throw CoreError::invalid("empty tile");
    const double sf = lp.scale;
    const std::int64_t sx = static_cast<std::int64_t>(std::round(tx * t * sf));
    const std::int64_t sy = static_cast<std::int64_t>(std::round(ty * t * sf));
    const std::int64_t sw = std::max<std::int64_t>(1,
        static_cast<std::int64_t>(std::ceil(out_w * sf)));
    const std::int64_t sh = std::max<std::int64_t>(1,
        static_cast<std::int64_t>(std::ceil(out_h * sf)));
    const std::int64_t pad = (sf > 1.0) ? static_cast<std::int64_t>(std::ceil(sf)) : 1;
    const std::int64_t rx = sx - pad;
    const std::int64_t ry = sy - pad;
    const std::uint32_t rw = static_cast<std::uint32_t>(sw + pad*2);
    const std::uint32_t rh = static_cast<std::uint32_t>(sh + pad*2);
    auto buf = reader.readRect(rx, ry, rw, rh);
    QImage img(reinterpret_cast<const std::uint8_t*>(buf.data()),
               static_cast<int>(rw), static_cast<int>(rh),
               static_cast<int>(rw)*4, QImage::Format_RGBA8888);
    img = img.copy();
    // 裁回精确区域
    const std::int64_t cx = sx - rx;
    const std::int64_t cy = sy - ry;
    QImage cropped = img.copy(static_cast<int>(cx), static_cast<int>(cy),
                              static_cast<int>(sw), static_cast<int>(sh));
    if (static_cast<std::int64_t>(out_w) == sw && static_cast<std::int64_t>(out_h) == sh
        && std::abs(sf - 1.0) < 1e-9) {
        return std::vector<std::uint8_t>(cropped.bits(), cropped.bits() + cropped.sizeInBytes());
    }
    QImage scaled = cropped.scaled(static_cast<int>(out_w), static_cast<int>(out_h),
                                   Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (scaled.format() != QImage::Format_RGBA8888) {
        scaled = scaled.convertToFormat(QImage::Format_RGBA8888);
    }
    return std::vector<std::uint8_t>(scaled.bits(), scaled.bits() + scaled.sizeInBytes());
}

/// Mercator 单瓦片 (mip 模式).
static std::vector<std::uint8_t> renderTileMercatorFromMips(const FullMips& mips,
                                                            const CutParams& params,
                                                            const GeoRef& g,
                                                            std::uint32_t z, std::uint32_t tx, std::uint32_t ty) {
    const double t = params.tile_size;
    const double res = MERC_INIT_RESOLUTION / std::pow(2.0, static_cast<int>(z));
    const double wx_l = tx * t * res - MERC_ORIGIN_SHIFT;
    const double wx_r = (tx + 1) * t * res - MERC_ORIGIN_SHIFT;
    const double wy_top = MERC_ORIGIN_SHIFT - ty * t * res;
    const double wy_bot = MERC_ORIGIN_SHIFT - (ty + 1) * t * res;
    const double fx0 = (wx_l - g.mx0) / g.sx;
    const double fx1 = (wx_r - g.mx0) / g.sx;
    const double fy_top = (wy_top - g.my_top) / g.sy;
    const double fy_bot = (wy_bot - g.my_top) / g.sy;
    const std::int64_t x0 = static_cast<std::int64_t>(std::floor(std::min(fx0, fx1)));
    const std::int64_t x1 = static_cast<std::int64_t>(std::ceil(std::max(fx0, fx1)));
    const std::int64_t y0 = static_cast<std::int64_t>(std::floor(std::min(fy_top, fy_bot)));
    const std::int64_t y1 = static_cast<std::int64_t>(std::ceil(std::max(fy_top, fy_bot)));
    const std::int64_t rw_full = std::max<std::int64_t>(1, x1 - x0);
    const std::int64_t rh_full = std::max<std::int64_t>(1, y1 - y0);

    const std::int64_t span = std::max(rw_full, rh_full);
    const std::uint32_t k = pickK(span, static_cast<std::int64_t>(params.tile_size), mips.lv.size());
    const auto [mw, mh] = mips.dims[k];
    const auto& buf = *mips.lv[k];
    const std::int64_t k2 = (1LL << k);
    const std::int64_t cx0 = std::max<std::int64_t>(0, (x0 / k2) - 1);
    const std::int64_t cy0 = std::max<std::int64_t>(0, (y0 / k2) - 1);
    const std::int64_t cw = std::max<std::int64_t>(1, std::min<std::int64_t>((x1 / k2) + 1, static_cast<std::int64_t>(mw)) - cx0);
    const std::int64_t ch = std::max<std::int64_t>(1, std::min<std::int64_t>((y1 / k2) + 1, static_cast<std::int64_t>(mh)) - cy0);
    QImage mip(reinterpret_cast<const std::uint8_t*>(buf.data()),
               static_cast<int>(mw), static_cast<int>(mh), static_cast<int>(mw)*4,
               QImage::Format_RGBA8888);
    mip = mip.copy();
    QImage cropped = mip.copy(static_cast<int>(cx0), static_cast<int>(cy0),
                              static_cast<int>(cw), static_cast<int>(ch));
    QImage scaled = cropped.scaled(static_cast<int>(params.tile_size), static_cast<int>(params.tile_size),
                                   Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (scaled.format() != QImage::Format_RGBA8888) {
        scaled = scaled.convertToFormat(QImage::Format_RGBA8888);
    }
    return std::vector<std::uint8_t>(scaled.bits(), scaled.bits() + scaled.sizeInBytes());
}

/// 相对模式单瓦片 (mip 模式).
static std::vector<std::uint8_t> renderTileFromMips(const FullMips& mips,
                                                     const CutParams& params,
                                                     const LevelPlan& lp,
                                                     std::uint32_t tx, std::uint32_t ty) {
    const std::uint32_t t = params.tile_size;
    const std::uint32_t out_w = std::min<std::uint32_t>(t, lp.width - tx * t);
    const std::uint32_t out_h = std::min<std::uint32_t>(t, lp.height - ty * t);
    const double sf = lp.scale;
    const std::int64_t sx = static_cast<std::int64_t>(std::round(tx * t * sf));
    const std::int64_t sy = static_cast<std::int64_t>(std::round(ty * t * sf));
    const std::int64_t sw = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(out_w * sf)));
    const std::int64_t sh = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(out_h * sf)));

    const std::int64_t span = std::max(sw, sh);
    const std::uint32_t k = pickK(span, static_cast<std::int64_t>(std::max<std::uint32_t>(out_w, out_h)), mips.lv.size());
    const auto [mw, mh] = mips.dims[k];
    const auto& buf = *mips.lv[k];
    const std::int64_t k2 = (1LL << k);
    const std::int64_t cx0 = std::max<std::int64_t>(0, (sx / k2) - 1);
    const std::int64_t cy0 = std::max<std::int64_t>(0, (sy / k2) - 1);
    const std::int64_t cw = std::max<std::int64_t>(1, std::min<std::int64_t>(((sx + sw) / k2) + 1, static_cast<std::int64_t>(mw)) - cx0);
    const std::int64_t ch = std::max<std::int64_t>(1, std::min<std::int64_t>(((sy + sh) / k2) + 1, static_cast<std::int64_t>(mh)) - cy0);
    QImage mip(reinterpret_cast<const std::uint8_t*>(buf.data()),
               static_cast<int>(mw), static_cast<int>(mh), static_cast<int>(mw)*4,
               QImage::Format_RGBA8888);
    mip = mip.copy();
    QImage cropped = mip.copy(static_cast<int>(cx0), static_cast<int>(cy0),
                              static_cast<int>(cw), static_cast<int>(ch));
    QImage scaled = cropped.scaled(static_cast<int>(out_w), static_cast<int>(out_h),
                                   Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (scaled.format() != QImage::Format_RGBA8888) {
        scaled = scaled.convertToFormat(QImage::Format_RGBA8888);
    }
    return std::vector<std::uint8_t>(scaled.bits(), scaled.bits() + scaled.sizeInBytes());
}

/// 概览级 (gdal2tiles 风格): 从上一级 4 块子瓦片降采样合成.
static std::vector<std::uint8_t> renderTileOverview(const CutParams& params,
                                                     std::uint32_t z, std::uint32_t tx, std::uint32_t ty) {
    const std::uint32_t t = params.tile_size;
    QImage canvas(static_cast<int>(t*2), static_cast<int>(t*2), QImage::Format_RGBA8888);
    canvas.fill(0);
    for (unsigned dy = 0; dy < 2; ++dy) {
        for (unsigned dx = 0; dx < 2; ++dx) {
            const std::uint32_t cz = z + 1;
            const std::uint32_t ctx = tx*2 + dx;
            const std::uint32_t cty = ty*2 + dy;
            const auto rows = 1u << std::min<std::uint32_t>(cz, 30);
            const auto rel = tileRelPath(params.scheme, cz, ctx, cty, rows);
            const auto p = params.output / rel;
            if (!fs::exists(p)) continue;
            QImage child;
            if (!child.load(p.string().c_str())) continue;
            // QImage::load PNG 默认返回 Format_ARGB32_Premultiplied (BGRA 内存).
            // 显式转 RGBA8888 后再 paint, 否则 canvas 上是 BGRA, 后续 scaled 输出
            // 也是 BGRA, 直接 memcpy 给 stbi_write_png 会把字节当 RGBA 解释, 偏蓝/橙.
            if (child.format() != QImage::Format_RGBA8888) {
                child = child.convertToFormat(QImage::Format_RGBA8888);
            }
            // paint over canvas
            for (int y = 0; y < child.height(); ++y) {
                std::memcpy(canvas.scanLine(static_cast<int>(dy*t) + y) + dx*t*4,
                            child.constScanLine(y), static_cast<std::size_t>(child.bytesPerLine()));
            }
        }
    }
    QImage scaled = canvas.scaled(static_cast<int>(t), static_cast<int>(t),
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // scaled 实际返回 Format_ARGB32_Premultiplied (Qt 5.12 在缩放时为 premultiplied).
    // 转回 RGBA8888 保证 rgba 字节序 [R G B A] 与 stbi_write_png 期望一致.
    if (scaled.format() != QImage::Format_RGBA8888) {
        scaled = scaled.convertToFormat(QImage::Format_RGBA8888);
    }
    return std::vector<std::uint8_t>(scaled.bits(), scaled.bits() + scaled.sizeInBytes());
}

} // anonymous namespace

CutSummary runCut(const CutParams& params, CutSink sink) {
    return runCutWithControl(params, TaskControl::create(), std::move(sink));
}

CutSummary runCutWithControl(const CutParams& params,
                             std::shared_ptr<TaskControl> control,
                             CutSink sink) {
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();

    CutSummary summary;
    summary.output_dir = params.output.string();

    std::function<void(const CutEvent&)> emit_event = [&](const CutEvent& ev) {
        try { sink(ev); } catch (...) {}
    };

    auto finishWith = [&](std::string err) {
        summary.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started).count();
        summary.errors.push_back(std::move(err));
        emit_event(CutEvent::done(summary));
        return summary;
    };

    // ---- 准备 ----
    std::unique_ptr<SourceReader> reader_u;
    try {
        reader_u = std::make_unique<SourceReader>(params.source);
    } catch (const std::exception& e) {
        return finishWith(e.what());
    }
    const auto src_w = reader_u->width();
    const auto src_h = reader_u->height();

    std::optional<GeoRef> geo;
    std::array<double, 4> merc_bounds{};
    std::optional<PyramidPlan> pyramid;
    std::optional<MercPlan> mplan;
    if (params.mercator) {
        try {
            geo = probeGeoref(params.source);
        } catch (const std::exception& e) {
            return finishWith(e.what());
        }
        if (!geo) return finishWith("image missing georef");
        merc_bounds = geo->bounds3857(src_w, src_h);
        const double sx_m = (merc_bounds[2] - merc_bounds[0]) / src_w;
        try {
            mplan = planMercator(merc_bounds, sx_m, params.tile_size,
                                  params.zmin, params.zmax,
                                  MAX_TOTAL_TILES_HARD);
        } catch (const std::exception& e) {
            return finishWith(e.what());
        }
    } else {
        try {
            pyramid = planPyramid(src_w, src_h, params.tile_size, params.zmin, params.zmax);
        } catch (const std::exception& e) {
            return finishWith(e.what());
        }
    }

    try { ensureOutDir(params.output); }
    catch (const std::exception& e) { return finishWith(e.what()); }

    // ---- 任务列表 ----
    std::vector<Job> jobs;
    std::uint32_t base_z = 0;
    if (mplan) {
        // mercator: 最高级 (基础级) 先, 低级概览后
        for (auto it = mplan->levels.rbegin(); it != mplan->levels.rend(); ++it) {
            const auto& lv = *it;
            const std::uint32_t rows = 1u << std::min<std::uint32_t>(lv.z, 30);
            for (std::uint32_t ty = lv.ty0; ty <= lv.ty1; ++ty) {
                for (std::uint32_t tx = lv.tx0; tx <= lv.tx1; ++tx) {
                    Job j; j.z = lv.z; j.tx = tx; j.ty = ty; j.tiles_y = rows; j.plan_idx = 0;
                    jobs.push_back(j);
                }
            }
        }
        if (!mplan->levels.empty()) base_z = mplan->levels.back().z;
    } else {
        for (std::size_t pi = 0; pi < pyramid->levels.size(); ++pi) {
            const auto& lp = pyramid->levels[pi];
            for (std::uint32_t ty = 0; ty < lp.tiles_y; ++ty) {
                for (std::uint32_t tx = 0; tx < lp.tiles_x; ++tx) {
                    Job j; j.z = lp.level; j.tx = tx; j.ty = ty; j.tiles_y = lp.tiles_y; j.plan_idx = static_cast<std::uint32_t>(pi);
                    jobs.push_back(j);
                }
            }
        }
    }
    const auto total = jobs.size();
    summary.total_tiles = total;
    emit_event(CutEvent::start(total));

    // ---- 断点续切 ----
    std::set<fs::path> skip;
    {
        const auto manifest_p = params.output / MANIFEST_NAME;
        if (fs::exists(manifest_p)) {
            std::ifstream f(manifest_p.string());
            std::stringstream ss; ss << f.rdbuf();
            try {
                auto j = nlohmann::json::parse(ss.str());
                bool same = j.value("source", std::string{}) == params.source.string()
                    && j.value("tile_size", 0) == params.tile_size
                    && j.value("scheme", std::string{}) == schemeToString(params.scheme);
                if (same) {
                    for (const auto& jb : jobs) {
                        const auto rel = tileRelPath(params.scheme, jb.z, jb.tx, jb.ty, jb.tiles_y);
                        const auto full = params.output / rel;
                        std::error_code ec;
                        const auto sz = fs::file_size(full, ec);
                        if (!ec && sz > 0) skip.insert(rel);
                    }
                }
            } catch (...) {}
        }
    }

    // ---- 共享状态 ----
    std::atomic<std::uint64_t> done_tiles{0};
    std::atomic<std::uint64_t> done_bytes{0};
    std::atomic<std::uint32_t> current_level{std::numeric_limits<std::uint32_t>::max()};
    std::atomic<bool> workers_done{false};
    std::mutex err_mu;
    std::vector<std::string> errors;
    constexpr std::size_t ERROR_CAP = 16;

    // ---- 巨型条带源: 读全图 ----
    std::optional<FullMips> mips;
    const auto giant = reader_u->giantStripBytes();
    const auto total_rgba = static_cast<std::uint64_t>(src_w) * src_h * 4;
    if (!control->cancel.load() && giant > 32ULL * 1024 * 1024
        && total_rgba <= 3ULL * 1024 * 1024 * 1024) {
        std::vector<std::uint8_t> full;
        if (!reader_u->readFullCancellable(full, &control->cancel)) {
            workers_done.store(true);
            summary.cancelled = true;
            summary.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started).count();
            emit_event(CutEvent::done(summary));
            return summary;
        }
        QImage img(reinterpret_cast<const std::uint8_t*>(full.data()),
                   static_cast<int>(src_w), static_cast<int>(src_h),
                   static_cast<int>(src_w)*4, QImage::Format_RGBA8888);
        mips = buildMips(img);
    }
    // ---- 进度心跳 ----
    std::thread ticker_thread([&]() {
        std::optional<std::uint32_t> last_level;
        while (!workers_done.load() && !control->cancel.load()) {
            // 用 QThread::msleep 代替 std::this_thread::sleep_for:
            // mingw730_64 (GCC 7.3.0) 的 libstdc++.a 在 -O3 下会把
            // sleep_for 内联成 nanosleep64, 但该运行时符号在 Qt 5.12
            // 自带的 mingw-w64 v5 中不存在, Release 链接报 undefined.
            QThread::msleep(120);
            const auto lv_raw = current_level.load();
            const auto lv = (lv_raw == std::numeric_limits<std::uint32_t>::max()) ? 0u : lv_raw;
            if (!last_level || *last_level != lv) {
                last_level = lv;
                emit_event(CutEvent::levelStart(lv));
            }
            ProgressSnapshot p;
            p.level = lv;
            p.tiles_done = done_tiles.load();
            p.total_tiles = total;
            p.bytes_written = done_bytes.load();
            p.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started).count();
            emit_event(CutEvent::progress(p));
        }
    });

    // ---- 任务处理 ----
    // 单瓦片处理 worker. 每 worker 拿到独立 reader 句柄 (无 mip 路径) 或共享 mip
    // (只读, 无竞争). writeTile 内部 PNG 编码走 g_png_encode_mu 串行化, 写盘每瓦片
    // 独立文件. 错误计数 + cancel 状态用 err_mu / atomic 保护.
    const unsigned hw = std::max<unsigned>(1, std::thread::hardware_concurrency());
    // 没有 mip 时为每 worker 独立 reader. 有 mip 时 reader_u 仅在
    // renderTileMercatorPrecise 用到, 仍然多 reader 更稳.
    std::vector<std::unique_ptr<SourceReader>> worker_readers;
    if (hw > 1) {
        for (unsigned i = 0; i < hw; ++i) {
            try { worker_readers.push_back(std::make_unique<SourceReader>(params.source)); }
            catch (...) { worker_readers.push_back(nullptr); }
        }
    }

    auto process_job_with_reader = [&](SourceReader* reader_override, const Job& job) {
        while (control->paused.load() && !control->cancel.load())
            QThread::msleep(120);
        if (control->cancel.load()) return;
        current_level.store(job.z);
        const auto rel = tileRelPath(params.scheme, job.z, job.tx, job.ty, job.tiles_y);
        if (skip.count(rel)) {
            done_tiles.fetch_add(1);
            return;
        }

        std::vector<std::uint8_t> rgba;
        SourceReader* use_reader = reader_override ? reader_override : reader_u.get();
        try {
            if (mplan && job.z < base_z) {
                rgba = renderTileOverview(params, job.z, job.tx, job.ty);
            } else if (mips) {
                if (mplan) {
                    if (params.precise && geo->src_epsg != 3857) {
                        rgba = renderTileMercatorPrecise(*use_reader, params, *geo, job.z, job.tx, job.ty);
                    } else {
                        rgba = renderTileMercatorFromMips(*mips, params, *geo, job.z, job.tx, job.ty);
                    }
                } else {
                    rgba = renderTileFromMips(*mips, params, pyramid->levels[job.plan_idx], job.tx, job.ty);
                }
            } else if (mplan) {
                if (params.precise && geo->src_epsg != 3857) {
                    rgba = renderTileMercatorPrecise(*use_reader, params, *geo, job.z, job.tx, job.ty);
                } else {
                    rgba = renderTileMercator(*use_reader, params, *geo, job.z, job.tx, job.ty);
                }
            } else {
                rgba = renderTile(*use_reader, params, pyramid->levels[job.plan_idx], job.tx, job.ty);
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(err_mu);
            if (errors.size() < ERROR_CAP) {
                errors.push_back("Z" + std::to_string(job.z) + " tile("
                  + std::to_string(job.tx) + "," + std::to_string(job.ty)
                  + ") failed: " + e.what());
            }
            control->cancel.store(true);
            return;
        }

        std::uint64_t bytes = 0;
        try {
            std::uint32_t w = 0, h = 0;
            const std::uint32_t n_px_total = static_cast<std::uint32_t>(rgba.size() / 4);
            if (n_px_total == 0) throw CoreError::invalid("empty tile");
            const std::uint32_t side_try = params.tile_size;
            if (n_px_total == side_try * side_try) {
                w = h = side_try;
            } else {
                w = side_try;
                h = n_px_total / w;
                if (h == 0 || w * h != n_px_total) {
                    h = side_try;
                    w = n_px_total / h;
                    if (w == 0 || w * h != n_px_total) {
                        for (w = 1; w <= side_try; ++w) {
                            if (n_px_total % w == 0) {
                                h = n_px_total / w;
                                if (h <= side_try) break;
                            }
                        }
                        if (w > side_try) {
                            throw CoreError::invalid("tile rgba size not parseable: " + std::to_string(rgba.size()));
                        }
                    }
                }
            }
            bytes = writeTile(params, rgba.data(), w, h, rel);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(err_mu);
            if (errors.size() < ERROR_CAP) {
                errors.push_back("write Z" + std::to_string(job.z)
                  + " tile(" + std::to_string(job.tx) + "," + std::to_string(job.ty)
                  + ") failed: " + e.what());
            }
            control->cancel.store(true);
            return;
        }
        done_tiles.fetch_add(1);
        done_bytes.fetch_add(bytes);
    };

    // 启动 hw 个 worker 线程, 通过 atomic next_idx 领 job (work-stealing 简化版).
    // 每级独立工作池: mercator 模式基础级先完成再切概览级, 普通模式一次跑完.
    std::atomic<std::size_t> next_idx{0};
    if (mplan) {
        // mercator: 基础级先整体完成, 然后概览级 (需读基础级子瓦片)
        std::size_t idx = 0;
        std::vector<std::future<void>> futs;
        for (auto it = mplan->levels.rbegin(); it != mplan->levels.rend(); ++it) {
            const auto n = static_cast<std::size_t>(it->count());
            const std::size_t level_start = idx;
            const std::size_t level_end = idx + n;
            idx = level_end;
            // 在这一级内多 worker 并行
            next_idx.store(level_start);
            for (unsigned t = 0; t < hw; ++t) {
                futs.push_back(std::async(std::launch::async, [&, t]() {
                    SourceReader* wr = nullptr;
                    if (t < worker_readers.size()) wr = worker_readers[t].get();
                    while (true) {
                        if (control->cancel.load()) break;
                        const auto i = next_idx.fetch_add(1);
                        if (i >= level_end) break;
                        process_job_with_reader(wr, jobs[i]);
                    }
                }));
            }
            for (auto& f : futs) f.wait();
            futs.clear();
            if (control->cancel.load()) break;
        }
    } else {
        next_idx.store(0);
        std::vector<std::future<void>> futs;
        for (unsigned t = 0; t < hw; ++t) {
            futs.push_back(std::async(std::launch::async, [&, t]() {
                SourceReader* wr = nullptr;
                if (t < worker_readers.size()) wr = worker_readers[t].get();
                while (true) {
                    if (control->cancel.load()) break;
                    const auto i = next_idx.fetch_add(1);
                    if (i >= jobs.size()) break;
                    process_job_with_reader(wr, jobs[i]);
                }
            }));
        }
        for (auto& f : futs) f.wait();
    }

    workers_done.store(true);
    if (ticker_thread.joinable()) ticker_thread.join();

    // ---- 收尾 ----
    summary.cancelled = control->cancel.load();
    summary.total_tiles = done_tiles.load();
    summary.bytes_written = done_bytes.load();
    summary.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started).count();
    {
        std::lock_guard<std::mutex> lk(err_mu);
        summary.errors = errors;
    }

    // 构造 levels 摘要
    if (mplan) {
        for (const auto& lv : mplan->levels) {
            LevelSummary s;
            s.level = lv.z;
            s.tiles = lv.count();
            const auto nx = lv.tx1 - lv.tx0 + 1;
            const auto ny = lv.ty1 - lv.ty0 + 1;
            s.width = static_cast<std::uint32_t>(nx) * params.tile_size;
            s.height = static_cast<std::uint32_t>(ny) * params.tile_size;
            s.ox = lv.tx0;
            s.oy = lv.ty0;
            s.wy = 1u << std::min<std::uint32_t>(lv.z, 30);
            summary.levels.push_back(s);
        }
    } else {
        for (const auto& lp : pyramid->levels) {
            LevelSummary s;
            s.level = lp.level;
            s.width = lp.width;
            s.height = lp.height;
            s.tiles = static_cast<std::uint64_t>(lp.tiles_x) * lp.tiles_y;
            s.wy = lp.tiles_y;
            summary.levels.push_back(s);
        }
    }

    if (summary.errors.empty() && !summary.cancelled) {
        Manifest m;
        m.app = "QCutter";
        m.version = "0.1.0";
        m.source = params.source.string();
        m.source_width = src_w;
        m.source_height = src_h;
        m.tile_size = params.tile_size;
        m.scheme = schemeToString(params.scheme);
        m.min_level = mplan ? mplan->levels.front().z : pyramid->min_level_requested;
        m.max_level = mplan ? mplan->levels.back().z : pyramid->max_level_requested;
        m.total_tiles = summary.total_tiles;
        m.bytes_written = summary.bytes_written;
        m.levels.clear();
        for (const auto& ls : summary.levels) {
            ManifestLevel ml;
            ml.level = ls.level; ml.width = ls.width; ml.height = ls.height;
            ml.tiles = ls.tiles; ml.ox = ls.ox; ml.oy = ls.oy; ml.wy = ls.wy;
            m.levels.push_back(ml);
        }
        try { writeManifest(params.output, m); }
        catch (const std::exception& e) { summary.errors.push_back(std::string("manifest: ") + e.what()); }

        PreviewInfo pi;
        pi.source_w = src_w;
        pi.source_h = src_h;
        pi.tile_size = params.tile_size;
        pi.zmin = m.min_level;
        pi.zmax = m.max_level;
        pi.tms = (params.scheme == Scheme::Tms);
        pi.levels = m.levels;
        pi.overlays_json = params.preview_overlays;
        try { writePreviewHtml(params.output, pi); }
        catch (const std::exception& e) { summary.errors.push_back(std::string("preview: ") + e.what()); }
    }

    emit_event(CutEvent::done(summary));
    return summary;
}

} // namespace qcutter