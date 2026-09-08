// 输出目录布局与 manifest/preview.html 生成.
#pragma once
#include "engine/alpha.h"
#include "engine/planner.h"
#include <cstdint>
#include <experimental/filesystem>
#include <string>
#include <vector>

namespace qcutter {

namespace fs = std::experimental::filesystem;

inline constexpr const char* MANIFEST_NAME    = "manifest.json";
inline constexpr const char* PREVIEW_HTML_NAME = "preview.html";

/// 瓦片相对路径: {z}/{x}/{y}.png (y 是否翻转由 scheme 决定).
fs::path tileRelPath(Scheme scheme, std::uint32_t level,
                     std::uint32_t x, std::uint32_t y_in, std::uint32_t tiles_y);

void ensureOutDir(const fs::path& out);

struct ManifestLevel {
    std::uint32_t level = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t tiles = 0;
    std::uint32_t ox = 0;
    std::uint32_t oy = 0;
    std::uint32_t wy = 0;
};

struct Manifest {
    std::string app;
    std::string version;
    std::string source;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t tile_size = 0;
    std::string scheme;
    std::uint32_t min_level = 0;
    std::uint32_t max_level = 0;
    std::vector<ManifestLevel> levels;
    std::uint64_t total_tiles = 0;
    std::uint64_t bytes_written = 0;
};

void writeManifest(const fs::path& out, const Manifest& m);

struct PreviewInfo {
    std::uint32_t source_w = 0;
    std::uint32_t source_h = 0;
    std::uint32_t tile_size = 0;
    std::uint32_t zmin = 0;
    std::uint32_t zmax = 0;
    bool tms = false;
    std::vector<ManifestLevel> levels;
    std::string overlays_json;
};

void writePreviewHtml(const fs::path& out, const PreviewInfo& info);

} // namespace qcutter