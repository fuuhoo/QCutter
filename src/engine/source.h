// TIFF 源读取器: 分块 (strip/tile) + LRU 缓存, 对外提供任意矩形 → RGBA8.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <experimental/filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// libtiff forward decls (避免在头文件直接 include)
struct tiff;
typedef struct tiff TIFF;

namespace qcutter {

namespace fs = std::experimental::filesystem;

class SourceReader;

/// 单个分块的解码结果（RGBA8 行主序 + 实际尺寸）.
struct ChunkRGBA {
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0;     ///< 实际宽度 (边缘 chunk 可能 < 标称)
    std::uint32_t h = 0;
};

/// 通道信息
enum class SampleType { U8, U16, I8, I16, U32, I32, F32, F64 };
enum class ColorFormat { Gray, GrayA, RGB, RGBA, Palette, CMYK, Unknown };

struct ImageInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ColorFormat color = ColorFormat::Unknown;
    SampleType sample = SampleType::U8;
    std::uint16_t samples_per_pixel = 0;
    std::uint16_t bits_per_sample = 0;
    bool has_alpha = false;
    bool premultiplied = false;
    std::uint16_t compression = 0;
    bool chunked_tiles = false;
    std::uint32_t chunk_w = 0;
    std::uint32_t chunk_h = 0;
    std::uint32_t chunk_count = 0;
    std::uint32_t rgba_bytes() const { return width * height * 4; }
};

/// 单条带/单瓦片 TIFF 源读取器.
class SourceReader {
public:
    explicit SourceReader(const fs::path& path);
    ~SourceReader();

    SourceReader(const SourceReader&) = delete;
    SourceReader& operator=(const SourceReader&) = delete;

    const fs::path& path() const noexcept { return path_; }

    /// 仅打开文件读元数据. 一次 TIFF 头, 不解码像素.
    static ImageInfo probe(const fs::path& path);

    std::uint32_t width()  const { return info_.width;  }
    std::uint32_t height() const { return info_.height; }
    std::uint32_t chunk_w() const { return info_.chunk_w; }
    std::uint32_t chunk_h() const { return info_.chunk_h; }
    bool tiles() const { return info_.chunked_tiles; }

    /// 单条带字节数 (giant_strip_bytes). 若 > 该值则改走全图光栅.
    std::uint64_t giantStripBytes() const {
        if (info_.chunked_tiles) return 0;
        return static_cast<std::uint64_t>(info_.chunk_w) * info_.chunk_h * 4ULL;
    }

    /// 读取源矩形为 RGBA8 行主序. 越界区域填 0.
    /// 矩形单位: 源图像坐标 (左上为原点, x→右, y→下).
    /// rx/ry/rw/rh: 任意整数, 函数自动 clamp.
    std::vector<std::uint8_t> readRect(std::int64_t rx, std::int64_t ry,
                                       std::uint32_t rw, std::uint32_t rh);

    /// 公开单个 chunk 解码: 给上层 (如 makePreview) 逐 tile 控制流程使用.
    /// 失败抛 CoreError; libjpeg fatal abort 等 native crash 不能被 C++ 异常捕获,
    /// 上层应配合 SetUnhandledExceptionFilter / fork 等兜底.
    std::optional<ChunkRGBA> decodeChunkPublic(std::uint32_t idx) {
        return decodeChunk(idx);
    }

    /// 整图读为 RGBA8 一次性. 用于巨型条带源.
    /// cancel: 可选取消旗标, 命中即返回 false.
    bool readFullCancellable(std::vector<std::uint8_t>& out,
                            const std::atomic<bool>* cancel = nullptr);

    /// 异步单条带解码后复制到画布 (行主序, 大小为 w*h*4).
    /// 取消旗标: 命中即中止 (返回 false).
    bool readFullChunkedCancellable(std::vector<std::uint8_t>& out,
                                    const std::atomic<bool>* cancel = nullptr);

private:
    fs::path path_;
    TIFF* tif_ = nullptr;
    ImageInfo info_;
    // 几何缓存
    std::uint32_t chunk_count_ = 0;
    std::uint32_t chunks_across_ = 1;

    struct CachedChunk {
        ChunkRGBA data;
        std::uint32_t bytes = 0; ///< rgba 字节数
    };
    std::unordered_map<std::uint32_t, CachedChunk> cache_;
    std::deque<std::uint32_t> order_;
    std::size_t cache_bytes_ = 0;
    static constexpr std::size_t CACHE_BUDGET = 160 * 1024 * 1024;

    // 临时存储每个 chunk 的实际尺寸 (因边缘 chunk 可能 < 标称)
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> chunk_dims_;

    // 保护 tif_ 的 libtiff handle, 多 worker 线程必须串行访问.
    // libtiff 内部状态不是线程安全的, 多线程并发 TIFFReadEncodedTile 会 SIGSEGV.
    // 用 recursive_mutex 因为 ensureChunk() 内部调用 decodeChunk(), 二者都需加锁.
    mutable std::recursive_mutex tif_mu_;

    // 工具
    std::pair<std::uint32_t, std::uint32_t> chunkOrigin(std::uint32_t ci) const;
    std::uint32_t chunkIndexOfPixel(std::uint32_t x, std::uint32_t y) const;
    void ensureChunk(std::uint32_t idx);
    std::optional<ChunkRGBA> decodeChunk(std::uint32_t idx);
};

} // namespace qcutter