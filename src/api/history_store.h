// SQLite 任务历史持久化.
#pragma once
#include "engine/alpha.h"
#include "engine/planner.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace qcutter {

struct HistoryRec {
    std::uint64_t id = 0;
    std::string source;
    std::string output;
    std::uint32_t tile_size = 256;
    Scheme scheme = Scheme::Xyz;
    AlphaMode alpha;
    Resample resample = Resample::Bilinear;
    std::optional<std::uint32_t> zmin;
    std::optional<std::uint32_t> zmax;
    bool skip_empty = false;
    bool mercator = false;
    bool precise = true;
    std::string status;
    std::uint32_t level = 0;
    std::uint64_t tiles_done = 0;
    std::uint64_t total_tiles = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t elapsed_ms = 0;
    std::optional<std::string> error;
    std::uint64_t started_ms = 0;
    std::uint64_t finished_ms = 0;
    std::optional<std::string> bounds_json;
};

class HistoryStore {
public:
    static HistoryStore& instance();
    void init();
    std::vector<HistoryRec> load();
    void save(const std::vector<HistoryRec>& recs);

private:
    HistoryStore() = default;
    std::mutex mu_;
};

} // namespace qcutter