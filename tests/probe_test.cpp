// 直接调用 probeGeoref 测试, 不需要 UI
#include "engine/meta.h"
#include "engine/source.h"
#include "util/logger.h"
#include "util/paths.h"
#include <cstdio>
#include <cstdlib>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <tiff>\n", argv[0]);
        return 2;
    }
    fs::path p = argv[1];
    std::fprintf(stderr, "[test] reading %s\n", p.string().c_str());

    // 安装 GeoTIFF tags
    qcutter::installGeorefTags();
    std::fprintf(stderr, "[test] georef tags installed\n");

    // Step 1: SourceReader probe
    try {
        auto info = qcutter::SourceReader::probe(p);
        std::fprintf(stderr, "[test] SourceReader::probe OK: %ux%u spp=%u bps=%u chunks=%u\n",
                     info.width, info.height, info.spp, info.bps, info.chunk_count);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[test] SourceReader::probe FAILED: %s\n", e.what());
        return 1;
    }

    // Step 2: probeGeoref
    try {
        auto geo = qcutter::probeGeoref(p);
        if (geo) {
            std::fprintf(stderr, "[test] probeGeoref OK: mx0=%.3f my_top=%.3f sx=%.6f sy=%.6f epsg=%u\n",
                         geo->mx0, geo->my_top, geo->sx, geo->sy, geo->src_epsg);
        } else {
            std::fprintf(stderr, "[test] probeGeoref returned nullopt (no georef)\n");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[test] probeGeoref FAILED: %s\n", e.what());
        return 1;
    }

    // Step 3: simulate makePreview by reading 100 tiles
    std::fprintf(stderr, "[test] opening SourceReader for tile decode...\n");
    try {
        qcutter::SourceReader r(p);
        std::fprintf(stderr, "[test] SourceReader opened\n");
        for (std::uint32_t i = 0; i < 30; ++i) {
            auto c = r.decodeChunkPublic(i);
            if (c) {
                std::fprintf(stderr, "[test] decodeChunk(%u) OK actual_w=%u actual_h=%u rgba=%zu\n",
                             i, c->w, c->h, c->rgba.size());
            } else {
                std::fprintf(stderr, "[test] decodeChunk(%u) returned nullopt\n", i);
                break;
            }
        }
        std::fprintf(stderr, "[test] all 30 tiles decoded without crash\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[test] SourceReader/decodeChunk FAILED: %s\n", e.what());
        return 1;
    }
    return 0;
}
