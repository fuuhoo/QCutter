#include "engine/source.h"
#include "engine/error.h"
#include "engine/meta.h"
#include <tiffio.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace qcutter {

// ---------------- 颜色格式探测 ----------------

static ColorFormat detectColorFormat(std::uint16_t spp, std::uint16_t photo) {
    // TIFF photometric interpretation: 0=WhiteIsZero, 1=BlackIsZero, 2=RGB, 3=Palette,
    // 4=TransparencyMask, 5=CMYK, 6=YCbCr, 8=CIELab.
    const bool has_alpha = (spp == 2 || spp == 4);
    switch (photo) {
    case PHOTOMETRIC_MINISWHITE:
    case PHOTOMETRIC_MINISBLACK:
        return has_alpha ? ColorFormat::GrayA : ColorFormat::Gray;
    case PHOTOMETRIC_RGB:
        return has_alpha ? ColorFormat::RGBA : ColorFormat::RGB;
    case PHOTOMETRIC_PALETTE:
        return ColorFormat::Palette;
    case PHOTOMETRIC_YCBCR:
    case PHOTOMETRIC_CIELAB:
    case PHOTOMETRIC_ICCLAB:
    case PHOTOMETRIC_ITULAB:
    case PHOTOMETRIC_LOGL:
    case PHOTOMETRIC_LOGLUV:
        return ColorFormat::Unknown; // 暂不支持
    case PHOTOMETRIC_SEPARATED: // CMYK
        return ColorFormat::CMYK;
    default:
        return ColorFormat::Unknown;
    }
}

static SampleType detectSample(std::uint16_t bits, std::uint16_t sample_fmt) {
    if (sample_fmt == SAMPLEFORMAT_UINT) {
        if (bits <= 8)  return SampleType::U8;
        if (bits <= 16) return SampleType::U16;
        return SampleType::U32;
    }
    if (sample_fmt == SAMPLEFORMAT_INT) {
        if (bits <= 8)  return SampleType::I8;
        if (bits <= 16) return SampleType::I16;
        return SampleType::I32;
    }
    if (sample_fmt == SAMPLEFORMAT_IEEEFP) {
        return (bits == 64) ? SampleType::F64 : SampleType::F32;
    }
    return SampleType::U8;
}

// ---------------- 通道 → RGBA8 ----------------

namespace {
inline std::uint8_t clamp8(long long v) noexcept {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<std::uint8_t>(v);
}
inline std::uint8_t clamp8(unsigned long long v) noexcept {
    if (v > 255ULL) return 255;
    return static_cast<std::uint8_t>(v);
}
inline std::uint8_t clamp8(double v) noexcept {
    if (v < 0.0) return 0;
    if (v > 1.0) return 255;
    return static_cast<std::uint8_t>(std::round(v * 255.0));
}
template <typename T>
std::uint8_t toU8(T v) noexcept {
    if constexpr (std::is_floating_point_v<T>) return clamp8(double(v));
    else if constexpr (std::is_signed_v<T>) {
        constexpr long long mid = (1LL << (sizeof(T)*8 - 1));
        constexpr long long max = (mid - 1);
        return clamp8(((long long)v + mid) * 255 / max);
    } else {
        constexpr unsigned long long max = ~(0ULL);
        return clamp8((unsigned long long)v * 255ULL / max);
    }
}
} // anonymous

/// 把单 chunk 的原始数据 (按 spp 排布) 转换为 RGBA8 行主序.
static std::vector<std::uint8_t> convertToRgba(const std::uint8_t* raw, std::size_t nbytes,
                                              std::uint32_t w, std::uint32_t h,
                                              ColorFormat color, SampleType st,
                                              std::uint16_t spp, std::uint16_t bps) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
    const std::size_t n_px = static_cast<std::size_t>(w) * h;
    if (color == ColorFormat::RGBA) {
        const std::size_t copy_bytes = std::min(nbytes, n_px * 4);
        std::memcpy(out.data(), raw, copy_bytes);
    } else if (color == ColorFormat::RGB) {
        if (st == SampleType::U8) {
            for (std::size_t i = 0; i < n_px; ++i) {
                out[i*4+0] = raw[i*3+0];
                out[i*4+1] = raw[i*3+1];
                out[i*4+2] = raw[i*3+2];
                out[i*4+3] = 255;
            }
        } else if (st == SampleType::U16) {
            const auto* p16 = reinterpret_cast<const std::uint16_t*>(raw);
            for (std::size_t i = 0; i < n_px; ++i) {
                out[i*4+0] = static_cast<std::uint8_t>(p16[i*3+0] >> 8);
                out[i*4+1] = static_cast<std::uint8_t>(p16[i*3+1] >> 8);
                out[i*4+2] = static_cast<std::uint8_t>(p16[i*3+2] >> 8);
                out[i*4+3] = 255;
            }
        }
    } else if (color == ColorFormat::Gray) {
        const std::size_t bps_bytes = (bps + 7) / 8;
        for (std::size_t i = 0; i < n_px; ++i) {
            std::uint8_t g;
            const auto* p = raw + i * bps_bytes;
            if (st == SampleType::U8 || st == SampleType::I8) g = p[0];
            else if (st == SampleType::U16 || st == SampleType::I16) g = p[1];
            else if (st == SampleType::U32 || st == SampleType::I32) g = p[3];
            else if (st == SampleType::F32 || st == SampleType::F64) g = clamp8(*reinterpret_cast<const double*>(p));
            else g = p[0];
            out[i*4+0] = g; out[i*4+1] = g; out[i*4+2] = g; out[i*4+3] = 255;
        }
    } else if (color == ColorFormat::GrayA) {
        const std::size_t bps_bytes = (bps + 7) / 8;
        for (std::size_t i = 0; i < n_px; ++i) {
            std::uint8_t g, a;
            const auto* p = raw + i * bps_bytes * 2;
            if (st == SampleType::U8 || st == SampleType::I8) { g = p[0]; a = p[1]; }
            else if (st == SampleType::U16 || st == SampleType::I16) { g = p[1]; a = p[3]; }
            else { g = p[0]; a = 255; }
            out[i*4+0] = g; out[i*4+1] = g; out[i*4+2] = g; out[i*4+3] = a;
        }
    } else if (color == ColorFormat::CMYK) {
        if (st == SampleType::U8) {
            for (std::size_t i = 0; i < n_px; ++i) {
                out[i*4+0] = 255 - raw[i*4+0];
                out[i*4+1] = 255 - raw[i*4+1];
                out[i*4+2] = 255 - raw[i*4+2];
                out[i*4+3] = 255;
            }
        } else if (st == SampleType::U16) {
            const auto* p16 = reinterpret_cast<const std::uint16_t*>(raw);
            for (std::size_t i = 0; i < n_px; ++i) {
                out[i*4+0] = static_cast<std::uint8_t>(255 - (p16[i*4+0] >> 8));
                out[i*4+1] = static_cast<std::uint8_t>(255 - (p16[i*4+1] >> 8));
                out[i*4+2] = static_cast<std::uint8_t>(255 - (p16[i*4+2] >> 8));
                out[i*4+3] = 255;
            }
        }
    } else {
        // Palette / Unknown: 用零填充, 上层会报错
        std::fill(out.begin(), out.end(), 0);
    }
    return out;
}

// ---------------- SourceReader ----------------

SourceReader::SourceReader(const fs::path& path) : path_(path) {
    fprintf(stderr, "[QCutter] SourceReader::open %s\n", path.string().c_str());
    // 确保 GeoTIFF tags 在任何 TIFFOpen 之前已注册 (libtiff extender 仅触发一次)
    installGeorefTags();
    tif_ = TIFFOpen(path.string().c_str(), "r");
    if (!tif_) throw CoreError::io(path.string(), "TIFFOpen failed");
    fprintf(stderr, "[QCutter] TIFFOpen OK, reading fields...\n");

    std::uint32_t w = 0, h = 0;
    if (!TIFFGetField(tif_, TIFFTAG_IMAGEWIDTH, &w)
        || !TIFFGetField(tif_, TIFFTAG_IMAGELENGTH, &h)) {
        TIFFClose(tif_);
        throw CoreError::tiff("missing ImageWidth/ImageLength");
    }
    if (w == 0 || h == 0) {
        TIFFClose(tif_);
        throw CoreError::unsupported("image dimensions are zero");
    }
    info_.width = w;
    info_.height = h;

    std::uint16_t spp = 0, bps = 0, photo = 0, sample_fmt = SAMPLEFORMAT_UINT;
    TIFFGetFieldDefaulted(tif_, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif_, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif_, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(tif_, TIFFTAG_SAMPLEFORMAT, &sample_fmt);
    info_.samples_per_pixel = spp;
    info_.bits_per_sample = bps;
    info_.color = detectColorFormat(spp, photo);
    info_.sample = detectSample(bps, sample_fmt);
    info_.has_alpha = (spp == 2 || spp == 4);

    std::uint16_t extra_count = 0;
    std::uint16_t* extra_types = nullptr;
    if (TIFFGetField(tif_, TIFFTAG_EXTRASAMPLES, &extra_count, &extra_types)
        && extra_count > 0 && extra_types != nullptr) {
        // 0=unspecified, 1=associated(premultiplied), 2=unassociated
        info_.premultiplied = (extra_types[0] == EXTRASAMPLE_ASSOCALPHA);
    }

    std::uint16_t compression = COMPRESSION_NONE;
    TIFFGetFieldDefaulted(tif_, TIFFTAG_COMPRESSION, &compression);
    info_.compression = compression;

    // 分块类型
    std::uint32_t tw = 0, th = 0;
    if (TIFFGetField(tif_, TIFFTAG_TILEWIDTH, &tw)
        && TIFFGetField(tif_, TIFFTAG_TILELENGTH, &th)) {
        info_.chunked_tiles = true;
        info_.chunk_w = tw;
        info_.chunk_h = th;
    } else {
        info_.chunked_tiles = false;
        std::uint32_t rps = h;
        TIFFGetFieldDefaulted(tif_, TIFFTAG_ROWSPERSTRIP, &rps);
        info_.chunk_w = w;
        info_.chunk_h = std::clamp<std::uint32_t>(rps, 1, h);
    }

    if (info_.color == ColorFormat::Unknown) {
        TIFFClose(tif_);
        throw CoreError::unsupported("unsupported TIFF color format");
    }
    if (info_.color == ColorFormat::Palette) {
        TIFFClose(tif_);
        throw CoreError::unsupported("Palette TIFF not supported; convert to RGB/RGBA first");
    }

    // PlanarConfiguration 必须为 1 (chunked). 分平面暂不支持.
    std::uint16_t planar = PLANARCONFIG_CONTIG;
    TIFFGetFieldDefaulted(tif_, TIFFTAG_PLANARCONFIG, &planar);
    if (planar != PLANARCONFIG_CONTIG) {
        TIFFClose(tif_);
        throw CoreError::unsupported("planar TIFF not supported");
    }

    // 计算 chunk_count
    if (info_.chunked_tiles) {
        chunks_across_ = (info_.width + info_.chunk_w - 1) / info_.chunk_w;
        const std::uint32_t rows = (info_.height + info_.chunk_h - 1) / info_.chunk_h;
        chunk_count_ = chunks_across_ * rows;
    } else {
        chunks_across_ = 1;
        chunk_count_ = (info_.height + info_.chunk_h - 1) / info_.chunk_h;
    }
    info_.chunk_count = chunk_count_;
    fprintf(stderr, "[QCutter] SourceReader::open done: %ux%u spp=%u bps=%u chunks=%u\n",
            info_.width, info_.height, info_.samples_per_pixel, info_.bits_per_sample, chunk_count_);
}

SourceReader::~SourceReader() {
    if (tif_) {
        fprintf(stderr, "[QCutter] SourceReader::~SourceReader closing TIFF\n");
        TIFFClose(tif_);
    }
}

ImageInfo SourceReader::probe(const fs::path& path) {
    SourceReader r(path);
    return r.info_;
}

std::pair<std::uint32_t, std::uint32_t> SourceReader::chunkOrigin(std::uint32_t ci) const {
    if (info_.chunked_tiles) {
        return { (ci % chunks_across_) * info_.chunk_w,
                 (ci / chunks_across_) * info_.chunk_h };
    } else {
        return { 0u, ci * info_.chunk_h };
    }
}

std::uint32_t SourceReader::chunkIndexOfPixel(std::uint32_t x, std::uint32_t y) const {
    if (info_.chunked_tiles) {
        return (y / info_.chunk_h) * chunks_across_ + (x / info_.chunk_w);
    } else {
        return y / info_.chunk_h;
    }
}

std::optional<ChunkRGBA> SourceReader::decodeChunk(std::uint32_t idx) {
    fprintf(stderr, "[QCutter] decodeChunk(%u) chunk_count_=%u\n", idx, chunk_count_);
    if (idx >= chunk_count_) return std::nullopt;

    // 整个 decodeChunk 串行化: libtiff 状态 + cache_/chunk_dims_ 都非线程安全.
    // 多 worker 线程并发会 SIGSEGV in TIFFVSetField. 牺牲并发换稳定性.
    std::lock_guard<std::recursive_mutex> lk(tif_mu_);

    const auto [ox, oy] = chunkOrigin(idx);

    // 边缘 chunk 实际尺寸 (libtiff 提供 TIFFTileSize / TIFFStripSize 字节数)
    std::uint32_t tw = info_.chunk_w, th = info_.chunk_h;
    if (info_.chunked_tiles) {
        tw = std::min<std::uint32_t>(tw, info_.width - ox);
        th = std::min<std::uint32_t>(th, info_.height - oy);
    } else {
        th = std::min<std::uint32_t>(th, info_.height - oy);
        // strip 宽度恒为图像宽度
    }

    tmsize_t bytes_needed = 0;
    if (info_.chunked_tiles) {
        bytes_needed = TIFFTileSize(tif_);
    } else {
        // libtiff 4.x: TIFFStripSize 接受单参数; 旧版接受 (TIFF*, strip).
        // 旧版 (libtiff < 4.5) 调用: TIFFStripSize(tif_, idx)
        // 新版 (libtiff >= 4.5) 调用: TIFFStripSize(tif_)  (按当前 strip 索引)
        bytes_needed = TIFFStripSize(tif_);
    }
    if (bytes_needed <= 0) return std::nullopt;

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(bytes_needed));

    int ok = 0;
    if (info_.chunked_tiles) {
        ok = TIFFReadEncodedTile(tif_, idx, raw.data(), bytes_needed);
    } else {
        ok = TIFFReadEncodedStrip(tif_, idx, raw.data(), bytes_needed);
    }
    fprintf(stderr, "[QCutter]   decodeChunk(%u) bytes_needed=%td ok=%d\n", idx, (long)bytes_needed, ok);
    if (ok == -1) {
        throw CoreError::tiff("failed to decode chunk");
    }
    // libtiff 返回真实写入字节数 (可能 < bytes_needed)
    const std::size_t written = static_cast<std::size_t>(ok);
    const std::size_t real_bytes = std::min<std::size_t>(written, raw.size());

    // 计算实际写入像素数 (按 spp*bps/8 字节计算)
    const std::uint16_t bps = info_.bits_per_sample;
    const std::uint16_t spp = info_.samples_per_pixel;
    const std::size_t bytes_per_px = (bps / 8) * spp;
    std::uint32_t actual_w = tw, actual_h = th;
    if (bytes_per_px > 0) {
        const std::size_t pix = real_bytes / bytes_per_px;
        if (info_.chunked_tiles) {
            // tile: pix = w*h
            if (pix < static_cast<std::size_t>(tw) * th) {
                actual_h = static_cast<std::uint32_t>(pix / tw);
            }
        } else {
            // strip: pix = w*h, w=image width
            if (pix < static_cast<std::size_t>(info_.width) * th) {
                actual_h = static_cast<std::uint32_t>(pix / info_.width);
            }
        }
    }

    auto rgba = convertToRgba(raw.data(), real_bytes,
                              info_.chunked_tiles ? actual_w : info_.width,
                              info_.chunked_tiles ? actual_h : actual_h,
                              info_.color, info_.sample, spp, bps);
    fprintf(stderr, "[QCutter]   decodeChunk(%u) rgba=%zu bytes (actual_w=%u actual_h=%u)\n",
            idx, rgba.size(), actual_w, actual_h);
    chunk_dims_[idx] = { actual_w, actual_h };
    ChunkRGBA c;
    c.rgba = std::move(rgba);
    c.w = info_.chunked_tiles ? actual_w : info_.width;
    c.h = actual_h;
    return c;
}

void SourceReader::ensureChunk(std::uint32_t idx) {
    // ensureChunk + decodeChunk 必须串行化 (cache_/order_/chunk_dims_ 都不是线程安全)
    std::lock_guard<std::recursive_mutex> lk(tif_mu_);
    if (cache_.count(idx)) return;
    auto decoded = decodeChunk(idx);
    if (!decoded) {
        throw CoreError::tiff("chunk " + std::to_string(idx) + " decode returned empty");
    }
    const auto bytes = decoded->rgba.size();

    // LRU 淘汰, 至少保留一个
    while (cache_bytes_ + bytes > CACHE_BUDGET && order_.size() > 1) {
        auto victim = order_.front(); order_.pop_front();
        auto it = cache_.find(victim);
        if (it != cache_.end()) {
            cache_bytes_ -= it->second.bytes;
            cache_.erase(it);
        }
        chunk_dims_.erase(victim);
    }
    CachedChunk cc;
    cc.data = std::move(*decoded);
    cc.bytes = static_cast<std::uint32_t>(bytes);
    cache_.emplace(idx, std::move(cc));
    cache_bytes_ += bytes;
    order_.push_back(idx);
}

std::vector<std::uint8_t> SourceReader::readRect(std::int64_t rx, std::int64_t ry,
                                                  std::uint32_t rw, std::uint32_t rh) {
    if (rw == 0 || rh == 0) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(rw) * rh * 4);
    if (rx >= static_cast<std::int64_t>(info_.width)
        || ry >= static_cast<std::int64_t>(info_.height)
        || rx + static_cast<std::int64_t>(rw) <= 0
        || ry + static_cast<std::int64_t>(rh) <= 0) {
        return out; // 完全在图外, 全部填 0
    }
    const std::int64_t x0 = std::clamp<std::int64_t>(rx, 0, static_cast<std::int64_t>(info_.width) - 1);
    const std::int64_t y0 = std::clamp<std::int64_t>(ry, 0, static_cast<std::int64_t>(info_.height) - 1);
    const std::int64_t x1 = std::min<std::int64_t>(rx + rw - 1, info_.width - 1);
    const std::int64_t y1 = std::min<std::int64_t>(ry + rh - 1, info_.height - 1);
    const std::uint32_t x0u = static_cast<std::uint32_t>(x0);
    const std::uint32_t y0u = static_cast<std::uint32_t>(y0);
    const std::uint32_t x1u = static_cast<std::uint32_t>(x1);
    const std::uint32_t y1u = static_cast<std::uint32_t>(y1);

    const std::uint32_t cw = info_.chunk_w;
    const std::uint32_t ch = info_.chunk_h;

    for (std::uint32_t cy = y0u / ch; cy <= y1u / ch; ++cy) {
        for (std::uint32_t cx = x0u / cw; cx <= x1u / cw; ++cx) {
            std::uint32_t idx;
            if (info_.chunked_tiles) {
                idx = cy * chunks_across_ + cx;
            } else {
                idx = cy;
            }
            if (idx >= chunk_count_) continue;
            ensureChunk(idx);
            const auto& cdat = cache_.at(idx).data;
            const auto ox = cx * cw;
            const auto oy = cy * ch;
            const auto& [cw_eff, ch_eff] = chunk_dims_.at(idx);
            const std::uint32_t sx = std::max(ox, x0u);
            const std::uint32_t sy = std::max(oy, y0u);
            const std::uint32_t ex = std::min<std::uint32_t>(ox + cw_eff, x1u + 1);
            const std::uint32_t ey = std::min<std::uint32_t>(oy + ch_eff, y1u + 1);
            if (sx >= ex || sy >= ey) continue;
            for (std::uint32_t row = sy; row < ey; ++row) {
                const auto src_off = ((row - oy) * static_cast<std::uint32_t>(cw_eff) + (sx - ox)) * 4;
                const auto dst_row = static_cast<std::size_t>(row) - static_cast<std::size_t>(ry);
                const auto dst_col = static_cast<std::size_t>(sx) - static_cast<std::size_t>(rx);
                const auto dst_off = (dst_row * rw + dst_col) * 4;
                const auto span = static_cast<std::size_t>(ex - sx) * 4;
                std::memcpy(out.data() + dst_off, cdat.rgba.data() + src_off, span);
            }
        }
    }
    return out;
}

bool SourceReader::readFullCancellable(std::vector<std::uint8_t>& out,
                                        const std::atomic<bool>* cancel) {
    out.assign(static_cast<std::size_t>(info_.width) * info_.height * 4, 0);
    for (std::uint32_t ci = 0; ci < chunk_count_; ++ci) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        ensureChunk(ci);
        const auto& cd = cache_.at(ci).data;
        const auto [ox, oy] = chunkOrigin(ci);
        const auto& [dw, dh] = chunk_dims_.at(ci);
        const std::uint32_t cw_eff = info_.chunked_tiles ? dw : info_.width;
        const std::uint32_t ch_eff = dh;
        for (std::uint32_t row = 0; row < ch_eff; ++row) {
            const auto src = row * cw_eff * 4;
            const auto dst_row = (oy + row) * info_.width + ox;
            const auto dst = dst_row * 4;
            const auto span = cw_eff * 4;
            std::memcpy(out.data() + dst, cd.rgba.data() + src, span);
        }
    }
    return true;
}

bool SourceReader::readFullChunkedCancellable(std::vector<std::uint8_t>& out,
                                              const std::atomic<bool>* cancel) {
    return readFullCancellable(out, cancel);
}

} // namespace qcutter