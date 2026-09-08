// 切片执行器: 任务参数 + 进度回调 + 取消控制.
#pragma once
#include "engine/alpha.h"
#include "engine/meta.h"
#include "engine/planner.h"
#include <atomic>
#include <cstdint>
#include "util/fs_compat.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qcutter {

struct CutParams {
    fs::path source;
    fs::path output;
    std::uint32_t tile_size = 256;
    std::optional<std::uint32_t> zmin;
    std::optional<std::uint32_t> zmax;
    Scheme scheme = Scheme::Xyz;
    AlphaMode alpha = AlphaMode::keep();
    Resample resample = Resample::Bilinear;
    bool skip_empty = false;
    bool mercator = false;
    bool precise = true;
    std::string preview_overlays; // JSON 数组; 空 = 无

    /// 兼容 QDataStream / QSettings 的序列化
};

struct ProgressSnapshot {
    std::uint32_t level = 0;
    std::uint64_t tiles_done = 0;
    std::uint64_t total_tiles = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t elapsed_ms = 0;
};

struct LevelSummary {
    std::uint32_t level = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t tiles = 0;
    std::uint32_t ox = 0;
    std::uint32_t oy = 0;
    std::uint32_t wy = 0;
};

struct CutSummary {
    std::string output_dir;
    std::uint64_t total_tiles = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t elapsed_ms = 0;
    bool cancelled = false;
    std::vector<std::string> errors;
    std::vector<LevelSummary> levels;
};

struct CutEvent {
    enum class Kind { Start, LevelStart, Progress, Done };
    Kind kind = Kind::Start;
    std::uint64_t total_tiles = 0;
    std::uint32_t level = 0;
    std::uint64_t tiles_done = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t elapsed_ms = 0;
    CutSummary summary;

    static CutEvent start(std::uint64_t n) { CutEvent e; e.kind = Kind::Start; e.total_tiles = n; return e; }
    static CutEvent levelStart(std::uint32_t lv) { CutEvent e; e.kind = Kind::LevelStart; e.level = lv; return e; }
    static CutEvent progress(ProgressSnapshot p) {
        CutEvent e; e.kind = Kind::Progress;
        e.level = p.level; e.tiles_done = p.tiles_done; e.total_tiles = p.total_tiles;
        e.bytes_written = p.bytes_written; e.elapsed_ms = p.elapsed_ms;
        return e;
    }
    static CutEvent done(CutSummary s) { CutEvent e; e.kind = Kind::Done; e.summary = std::move(s); return e; }
};

/// 任务控制: 取消 + 暂停.
struct TaskControl {
    std::atomic<bool> cancel{false};
    std::atomic<bool> paused{false};
    static std::shared_ptr<TaskControl> create() { return std::make_shared<TaskControl>(); }
};

/// 事件回调 (线程安全, 由 cutter 内部串行调用).
using CutSink = std::function<void(const CutEvent&)>;

/// 执行切片 (同步阻塞). 同一线程内运行 sink.
CutSummary runCut(const CutParams& params, CutSink sink);

/// 带控制句柄的版本 (支持取消/暂停).
CutSummary runCutWithControl(const CutParams& params,
                             std::shared_ptr<TaskControl> control,
                             CutSink sink);

/// 简易进度回调包装器 (用于 Qt signals/slots).
class CutCallbacks {
public:
    std::function<void(std::uint64_t total)> onStart;
    std::function<void(std::uint32_t level)> onLevelStart;
    std::function<void(ProgressSnapshot)> onProgress;
    std::function<void(CutSummary)> onDone;
    CutSink build() {
        return [this](const CutEvent& ev) {
            switch (ev.kind) {
            case CutEvent::Kind::Start:
                if (onStart) onStart(ev.total_tiles);
                break;
            case CutEvent::Kind::LevelStart:
                if (onLevelStart) onLevelStart(ev.level);
                break;
            case CutEvent::Kind::Progress: {
                ProgressSnapshot p;
                p.level = ev.level;
                p.tiles_done = ev.tiles_done;
                p.total_tiles = ev.total_tiles;
                p.bytes_written = ev.bytes_written;
                p.elapsed_ms = ev.elapsed_ms;
                if (onProgress) onProgress(p);
                break;
            }
            case CutEvent::Kind::Done:
                if (onDone) onDone(ev.summary);
                break;
            }
        };
    }
};

} // namespace qcutter