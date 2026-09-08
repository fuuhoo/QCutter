// 任务管理: 创建任务 / 并发调度 / 进度事件流 / 取消 / 暂停 / 历史持久化.
#pragma once
#include "engine/alpha.h"
#include "engine/cutter.h"
#include "engine/planner.h"
#include "api/history_store.h"
#include <QMetaType>
#include <QObject>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace qcutter {

/// 一个切片任务的全部配置.
struct TaskConfig {
    QString source;
    QString output;
    std::uint32_t tile_size = 256;
    std::optional<std::uint32_t> zmin;
    std::optional<std::uint32_t> zmax;
    Scheme scheme = Scheme::Xyz;
    AlphaMode alpha = AlphaMode::keep();
    Resample resample = Resample::Bilinear;
    bool skip_empty = false;
    bool mercator = false;
    bool precise = true;
    QString preview_overlays; // JSON; 空 = 无
};

/// 图像元信息 + 默认级别估算.
struct ImageBrief {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    QString pixel_format;
    std::uint16_t bits_per_sample = 0;
    QString compression;
    QString chunk_type;
    std::uint32_t chunk_w = 0;
    std::uint32_t chunk_h = 0;
    bool has_alpha = false;
    std::uint64_t rgba_bytes = 0;
    std::uint32_t max_level = 0;
};

struct LevelEstimate {
    std::uint32_t level = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;
    std::uint64_t tiles = 0;
};

struct PyramidEstimate {
    std::optional<std::uint32_t> native_zoom;
    std::vector<LevelEstimate> levels;
};

struct TaskSummary {
    std::uint64_t tiles_done = 0;
    std::uint64_t total_tiles = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t elapsed_ms = 0;
    bool cancelled = false;
    std::optional<QString> error;
};

struct TaskDto {
    std::uint64_t id = 0;
    QString source;
    QString output;
    std::uint32_t tile_size = 256;
    Scheme scheme = Scheme::Xyz;
    AlphaMode alpha;
    Resample resample = Resample::Bilinear;
    std::optional<std::uint32_t> zmin;
    std::optional<std::uint32_t> zmax;
    QString status;
    std::uint32_t level = 0;
    std::uint64_t tiles_done = 0;
    std::uint64_t total_tiles = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t elapsed_ms = 0;
    std::optional<QString> error;
    std::uint64_t started_at_ms = 0;
    std::uint64_t finished_at_ms = 0;
};

/// 推送给 Qt 的任务事件.
struct TaskEvent {
    std::uint64_t task_id = 0;
    enum class Kind { StatusChanged, Started, LevelStart, Progress, Finished, Removed } kind = Kind::StatusChanged;
    QString status;
    std::uint64_t total_tiles = 0;
    std::uint32_t level = 0;
    std::uint64_t tiles_done = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t elapsed_ms = 0;
    TaskSummary summary;

    static TaskEvent statusChanged(std::uint64_t id, const QString& st) {
        TaskEvent e; e.task_id = id; e.kind = Kind::StatusChanged; e.status = st; return e;
    }
    static TaskEvent removed(std::uint64_t id) {
        TaskEvent e; e.task_id = id; e.kind = Kind::Removed; return e;
    }
    static TaskEvent started(std::uint64_t id, std::uint64_t total) {
        TaskEvent e; e.task_id = id; e.kind = Kind::Started; e.total_tiles = total; return e;
    }
    static TaskEvent levelStart(std::uint64_t id, std::uint32_t lv) {
        TaskEvent e; e.task_id = id; e.kind = Kind::LevelStart; e.level = lv; return e;
    }
    static TaskEvent progress(std::uint64_t id, const ProgressSnapshot& p) {
        TaskEvent e; e.task_id = id; e.kind = Kind::Progress;
        e.level = p.level; e.tiles_done = p.tiles_done; e.total_tiles = p.total_tiles;
        e.bytes_written = p.bytes_written; e.elapsed_ms = p.elapsed_ms;
        return e;
    }
    static TaskEvent finished(std::uint64_t id, TaskSummary s) {
        TaskEvent e; e.task_id = id; e.kind = Kind::Finished; e.summary = std::move(s); return e;
    }
};

class TaskManager : public QObject {
    Q_OBJECT
public:
    static TaskManager& instance();
    /// 仅供 cpp 内部使用 (Pimpl 广播事件)
    static TaskManager* instancePtr();

    /// 启动时初始化: 加载历史 / 设置默认并发度.
    void bootstrap();
    void shutdown();

    // -- 信息接口 --
    static ImageBrief readImageInfo(const QString& path);
    static PyramidEstimate estimatePyramidEx(const QString& source, std::uint32_t width,
                                            std::uint32_t height, std::uint32_t tile_size,
                                            std::optional<std::uint32_t> zmin,
                                            std::optional<std::uint32_t> zmax,
                                            bool mercator);
    static QByteArray makePreview(const QString& source, std::uint32_t max_px, const AlphaMode& alpha);
    static std::vector<std::uint8_t> samplePixel(const QString& source, std::int64_t x, std::int64_t y);

    // -- 任务控制 --
    std::uint64_t startTask(const TaskConfig& cfg);
    bool cancelTask(std::uint64_t id);
    bool pauseTask(std::uint64_t id);
    bool resumeTask(std::uint64_t id);
    bool removeTask(std::uint64_t id);

    void setMaxConcurrency(std::uint32_t n);
    std::uint32_t maxConcurrency() const;

    std::vector<TaskDto> listTasks() const;

signals:
    void eventReady(qcutter::TaskEvent ev);

private:
    explicit TaskManager(QObject* parent = nullptr);
    ~TaskManager() override;
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace qcutter

Q_DECLARE_METATYPE(qcutter::TaskEvent)