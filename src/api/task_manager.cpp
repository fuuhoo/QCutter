#include "api/task_manager.h"
#include "engine/error.h"
#include "engine/mercator.h"
#include "engine/meta.h"
#include "engine/planner.h"
#include "engine/source.h"
#include "util/logger.h"
#include "util/paths.h"

#include <QImage>
#include <QBuffer>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QJsonArray>
#include <cstdio>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QThread>
#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace qcutter {

namespace fs = std::experimental::filesystem;

// ---------------- Preview chunk 缓存 (进程级) ----------------
// swCutter 风格: 同一 TIFF 的 chunk RGBA 跨多次 makePreview 调用共享,
// 重复预览只采样像素不解码. 4GB 字节预算, FIFO 淘汰.
//
// 进程级缓存需要满足:
//   1. 多个 makePreview 同时跑也安全 (mutex 串行访问 entries_)
//   2. 不同路径不同 ci 独立存储
//   3. 内存上限可控

namespace preview_cache_internal {

struct ChunkKey {
    fs::path path;
    std::uint32_t ci;
    bool operator==(const ChunkKey& o) const noexcept {
        return ci == o.ci && path == o.path;
    }
};

struct ChunkEntry {
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

constexpr std::uint64_t PREVIEW_BUDGET_BYTES = 4ULL * 1024 * 1024 * 1024;  // 4GB

struct CacheState {
    std::mutex mu;
    std::vector<std::pair<ChunkKey, ChunkEntry>> entries;  // FIFO, head=oldest
    std::uint64_t bytes = 0;
};

static CacheState& cache_state() {
    static CacheState s;
    return s;
}

/// 查询/插入一个 chunk. 命中返回 true, rgba/w/h 已填充.
/// 失败 (oom/decode) 返回 false 并插入 nullptr 标记以避免重复尝试, 5s 后允许重试.
static bool get_or_insert(const fs::path& path, std::uint32_t ci,
                          const std::function<ChunkEntry()>& decode,
                          ChunkEntry& out) {
    auto& st = cache_state();
    ChunkKey key{path, ci};
    {
        std::lock_guard<std::mutex> lk(st.mu);
        for (auto& e : st.entries) {
            if (e.first == key) {
                out = e.second;
                return !out.rgba.empty();
            }
        }
    }
    // 不在缓存: 解码 (在锁外, libtiff 不能并发同 handle, 但不同 ci 不同 worker 各自打开)
    ChunkEntry e = decode();
    {
        std::lock_guard<std::mutex> lk(st.mu);
        // 双重检查: 其他线程可能已经插入
        for (auto& ex : st.entries) {
            if (ex.first == key) {
                out = ex.second;
                return !out.rgba.empty();
            }
        }
        if (!e.rgba.empty()) {
            st.bytes += e.rgba.size();
            st.entries.push_back({key, std::move(e)});
            while (st.bytes > PREVIEW_BUDGET_BYTES && st.entries.size() > 1) {
                auto oldest = std::move(st.entries.front());
                st.entries.erase(st.entries.begin());
                st.bytes -= oldest.second.rgba.size();
            }
            for (auto& ex : st.entries) {
                if (ex.first == key) {
                    out = ex.second;
                    return true;
                }
            }
        }
    }
    out = std::move(e);
    return !out.rgba.empty();
}

static void clear_for_test() {
    auto& st = cache_state();
    std::lock_guard<std::mutex> lk(st.mu);
    st.entries.clear();
    st.bytes = 0;
}

}  // namespace preview_cache_internal

// ---------------- Impl ----------------

struct TaskManager::Impl {
    struct Entry {
        std::uint64_t id = 0;
        TaskConfig cfg;
        QString status;
        std::shared_ptr<TaskControl> control = TaskControl::create();
        std::atomic<bool> cancel{false};
        std::uint32_t level = 0;
        std::uint64_t tiles_done = 0;
        std::uint64_t total_tiles = 0;
        std::uint64_t bytes_written = 0;
        std::uint64_t elapsed_ms = 0;
        std::optional<QString> error;
        std::uint64_t started_ms = 0;
        std::uint64_t finished_ms = 0;
        QString bounds_json;
        std::optional<QString> terminal_error;
        std::chrono::steady_clock::time_point started_at{};
        std::mutex mu;
        std::condition_variable cv;
        bool wait_started = false;
        bool wait_finished = false;
    };
    std::vector<std::shared_ptr<Entry>> entries;
    std::uint64_t next_id = 1;
    std::uint32_t max_concurrency = 2;
    std::uint32_t running = 0;
    std::mutex mgr_mu;
    std::condition_variable mgr_cv;

    std::shared_ptr<Entry> findById(std::uint64_t id) {
        for (auto& e : entries) if (e->id == id) return e;
        return nullptr;
    }

    void persistAll() {
        std::vector<HistoryRec> recs;
        recs.reserve(entries.size());
        for (const auto& e : entries) {
            HistoryRec r;
            r.id = e->id;
            r.source = e->cfg.source.toStdString();
            r.output = e->cfg.output.toStdString();
            r.tile_size = e->cfg.tile_size;
            r.scheme = e->cfg.scheme;
            r.alpha = e->cfg.alpha;
            r.resample = e->cfg.resample;
            r.zmin = e->cfg.zmin;
            r.zmax = e->cfg.zmax;
            r.skip_empty = e->cfg.skip_empty;
            r.mercator = e->cfg.mercator;
            r.precise = e->cfg.precise;
            r.status = e->status.toStdString();
            r.level = e->level;
            r.tiles_done = e->tiles_done;
            r.total_tiles = e->total_tiles;
            r.bytes_written = e->bytes_written;
            r.elapsed_ms = e->elapsed_ms;
            r.error = e->error ? std::optional<std::string>(e->error->toStdString()) : std::nullopt;
            r.started_ms = e->started_ms;
            r.finished_ms = e->finished_ms;
            if (!e->bounds_json.isEmpty())
                r.bounds_json = e->bounds_json.toStdString();
            recs.push_back(std::move(r));
        }
        HistoryStore::instance().save(recs);
    }

    void broadcast(TaskEvent ev) {
        if (auto* mgr = TaskManager::instancePtr()) {
            QMetaObject::invokeMethod(mgr, [mgr, ev = std::move(ev)]() {
                emit mgr->eventReady(ev);
            }, Qt::QueuedConnection);
        }
    }

    void worker(std::shared_ptr<Entry> e) {
        // 排队等槽
        std::unique_lock<std::mutex> lk(mgr_mu);
        mgr_cv.wait(lk, [&]() {
            if (e->cancel.load()) return true;
            return running < max_concurrency;
        });
        if (e->cancel.load()) {
            // 取消 (还在排)
            lk.unlock();
            e->status = "cancelled";
            e->finished_ms = nowMs();
            persistAll();
            broadcast(TaskEvent::statusChanged(e->id, e->status));
            broadcast(TaskEvent::finished(e->id, buildSummary(e, true)));
            return;
        }
        running += 1;
        lk.unlock();
        struct Release {
            Impl* mgr;
            ~Release() {
                std::lock_guard<std::mutex> lk(mgr->mgr_mu);
                mgr->running -= 1;
                mgr->mgr_cv.notify_all();
            }
        } _rel{this};

        e->started_at = std::chrono::steady_clock::now();
        e->started_ms = nowMs();
        e->status = "running";
        persistAll();
        broadcast(TaskEvent::statusChanged(e->id, e->status));

        CutParams params;
        params.source = fs::path(e->cfg.source.toStdString());
        params.output = fs::path(e->cfg.output.toStdString());
        params.tile_size = e->cfg.tile_size;
        params.zmin = e->cfg.zmin;
        params.zmax = e->cfg.zmax;
        params.scheme = e->cfg.scheme;
        params.alpha = e->cfg.alpha;
        params.resample = e->cfg.resample;
        params.skip_empty = e->cfg.skip_empty;
        params.mercator = e->cfg.mercator;
        params.precise = e->cfg.precise;
        params.preview_overlays = e->cfg.preview_overlays.toStdString();

        auto sink = [this, e](const CutEvent& ev) {
            switch (ev.kind) {
            case CutEvent::Kind::Start: {
                e->total_tiles = ev.total_tiles;
                broadcast(TaskEvent::started(e->id, ev.total_tiles));
                break;
            }
            case CutEvent::Kind::LevelStart: {
                e->level = ev.level;
                broadcast(TaskEvent::levelStart(e->id, ev.level));
                break;
            }
            case CutEvent::Kind::Progress: {
                e->level = ev.level;
                e->tiles_done = ev.tiles_done;
                e->total_tiles = ev.total_tiles;
                e->bytes_written = ev.bytes_written;
                e->elapsed_ms = ev.elapsed_ms;
                ProgressSnapshot p;
                p.level = ev.level;
                p.tiles_done = ev.tiles_done;
                p.total_tiles = ev.total_tiles;
                p.bytes_written = ev.bytes_written;
                p.elapsed_ms = ev.elapsed_ms;
                broadcast(TaskEvent::progress(e->id, p));
                break;
            }
            case CutEvent::Kind::Done: {
                break; // 由 worker 收尾统一上报
            }
            }
        };

        CutSummary summary;
        try {
            summary = runCutWithControl(params, e->control, sink);
        } catch (const std::exception& ex) {
            summary.errors.push_back(ex.what());
            summary.cancelled = false;
        }

        // bounds JSON
        QJsonArray levels_arr;
        for (const auto& l : summary.levels) {
            QJsonObject lo;
            lo["z"] = static_cast<int>(l.level);
            lo["ox"] = static_cast<int>(l.ox);
            lo["oy"] = static_cast<int>(l.oy);
            lo["wy"] = static_cast<int>(l.wy);
            lo["tx"] = static_cast<int>((l.width + params.tile_size - 1) / params.tile_size);
            lo["ty"] = static_cast<int>((l.height + params.tile_size - 1) / params.tile_size);
            levels_arr.append(lo);
        }
        QJsonObject bj;
        bj["scheme"] = QString::fromStdString(schemeToString(params.scheme));
        bj["tile"] = static_cast<int>(params.tile_size);
        bj["tms"] = (params.scheme == Scheme::Tms);
        if (!summary.levels.empty()) {
            bj["zmin"] = static_cast<int>(summary.levels.front().level);
            bj["zmax"] = static_cast<int>(summary.levels.back().level);
        }
        bj["levels"] = levels_arr;
        e->bounds_json = QString::fromUtf8(QJsonDocument(bj).toJson(QJsonDocument::Compact));

        e->tiles_done = summary.total_tiles;
        e->bytes_written = summary.bytes_written;
        e->elapsed_ms = summary.elapsed_ms;
        e->finished_ms = nowMs();
        if (!summary.errors.empty())
            e->error = QString::fromStdString(summary.errors.front());
        e->status = summary.cancelled ? "cancelled"
                   : (e->error ? "error" : "done");
        persistAll();
        broadcast(TaskEvent::statusChanged(e->id, e->status));
        broadcast(TaskEvent::finished(e->id, buildSummary(e, summary.cancelled)));
    }

    TaskSummary buildSummary(const std::shared_ptr<Entry>& e, bool cancelled) {
        TaskSummary s;
        s.tiles_done = e->tiles_done;
        s.total_tiles = e->total_tiles;
        s.bytes_written = e->bytes_written;
        s.elapsed_ms = e->elapsed_ms;
        s.cancelled = cancelled;
        if (e->error) s.error = *e->error;
        return s;
    }

    static std::uint64_t nowMs() {
        return QDateTime::currentMSecsSinceEpoch();
    }
};

TaskManager::TaskManager(QObject* parent) : QObject(parent), d_(std::make_unique<Impl>()) {}
TaskManager::~TaskManager() = default;
namespace { void _registerTaskEvent() { qRegisterMetaType<TaskEvent>("qcutter::TaskEvent"); } }

TaskManager& TaskManager::instance() {
    static TaskManager m;
    return m;
}
TaskManager* TaskManager::instancePtr() { return &instance(); }

void TaskManager::bootstrap() {
    HistoryStore::instance().init();
    // 加载历史为只读冷任务 (不会启动 worker)
    auto recs = HistoryStore::instance().load();
    for (auto& r : recs) {
        auto e = std::make_shared<Impl::Entry>();
        e->id = r.id;
        e->cfg.source = QString::fromStdString(r.source);
        e->cfg.output = QString::fromStdString(r.output);
        e->cfg.tile_size = r.tile_size;
        e->cfg.scheme = r.scheme;
        e->cfg.alpha = r.alpha;
        e->cfg.resample = r.resample;
        e->cfg.zmin = r.zmin;
        e->cfg.zmax = r.zmax;
        e->cfg.skip_empty = r.skip_empty;
        e->cfg.mercator = r.mercator;
        e->cfg.precise = r.precise;
        e->cfg.preview_overlays = QString::fromStdString("");
        // 上次未终态 → 标 cancelled 并注明
        const auto& st = r.status;
        if (st == "done" || st == "error" || st == "cancelled") {
            e->status = QString::fromStdString(st);
            e->error = r.error ? std::optional<QString>(QString::fromStdString(*r.error)) : std::nullopt;
        } else {
            e->status = "cancelled";
            e->error = QStringLiteral("应用退出导致中断（可重跑同参数续切）");
        }
        e->cancel.store(true);
        e->level = r.level;
        e->tiles_done = r.tiles_done;
        e->total_tiles = r.total_tiles;
        e->bytes_written = r.bytes_written;
        e->elapsed_ms = r.elapsed_ms;
        e->started_ms = r.started_ms;
        e->finished_ms = r.finished_ms;
        if (r.bounds_json) e->bounds_json = QString::fromStdString(*r.bounds_json);
        d_->entries.push_back(e);
        if (r.id >= d_->next_id) d_->next_id = r.id + 1;
    }
}

void TaskManager::shutdown() {
    d_->persistAll();
}

ImageBrief TaskManager::readImageInfo(const QString& path) {
    fprintf(stderr, "[QCutter] readImageInfo start: %s\n", path.toStdString().c_str());
    auto info = SourceReader::probe(fs::path(path.toStdString()));
    fprintf(stderr, "[QCutter] readImageInfo probed OK: %ux%u\n", info.width, info.height);
    ImageBrief b;
    b.width = info.width;
    b.height = info.height;
    switch (info.color) {
    case ColorFormat::Gray: b.pixel_format = "Gray"; break;
    case ColorFormat::GrayA: b.pixel_format = "GrayA"; break;
    case ColorFormat::RGB: b.pixel_format = "RGB"; break;
    case ColorFormat::RGBA: b.pixel_format = "RGBA"; break;
    case ColorFormat::CMYK: b.pixel_format = "CMYK"; break;
    case ColorFormat::Palette: b.pixel_format = "Palette"; break;
    default: b.pixel_format = "Unknown"; break;
    }
    b.pixel_format += QString::number(info.bits_per_sample);
    b.bits_per_sample = info.bits_per_sample;
    switch (info.compression) {
    case 1: b.compression = "None"; break;
    case 5: b.compression = "LZW"; break;
    case 7: b.compression = "JPEG"; break;
    case 8:
    case 32946: b.compression = "Deflate"; break;
    case 32773: b.compression = "PackBits"; break;
    default: b.compression = QString::number(info.compression); break;
    }
    b.chunk_type = info.chunked_tiles ? "tile" : "strip";
    b.chunk_w = info.chunk_w;
    b.chunk_h = info.chunk_h;
    b.has_alpha = info.has_alpha;
    b.rgba_bytes = info.rgba_bytes();
    b.max_level = nativeLevel(b.width, b.height, DEFAULT_TILE_SIZE);
    return b;
}

PyramidEstimate TaskManager::estimatePyramidEx(const QString& source, std::uint32_t width,
                                                std::uint32_t height, std::uint32_t tile_size,
                                                std::optional<std::uint32_t> zmin,
                                                std::optional<std::uint32_t> zmax,
                                                bool mercator) {
    PyramidEstimate pe;
    if (!mercator) {
        auto plan = planPyramid(width, height, tile_size, zmin, zmax);
        pe.native_zoom = nativeLevel(width, height, tile_size);
        for (const auto& lp : plan.levels) {
            LevelEstimate le;
            le.level = lp.level;
            le.width = lp.width; le.height = lp.height;
            le.tiles_x = lp.tiles_x; le.tiles_y = lp.tiles_y;
            le.tiles = static_cast<std::uint64_t>(lp.tiles_x) * lp.tiles_y;
            pe.levels.push_back(le);
        }
        return pe;
    }
    auto geo = probeGeoref(fs::path(source.toStdString()));
    if (!geo) throw CoreError::invalid("image missing georef");
    auto b = geo->bounds3857(width, height);
    const double sx_m = (b[2] - b[0]) / width;
    auto plan = planMercator(b, sx_m, tile_size, zmin, zmax, MAX_TOTAL_TILES_HARD);
    pe.native_zoom = plan.native_zoom;
    for (const auto& lv : plan.levels) {
        LevelEstimate le;
        le.level = lv.z;
        const auto nx = lv.tx1 - lv.tx0 + 1;
        const auto ny = lv.ty1 - lv.ty0 + 1;
        le.tiles_x = nx; le.tiles_y = ny;
        le.width = nx * tile_size; le.height = ny * tile_size;
        le.tiles = lv.count();
        pe.levels.push_back(le);
    }
    return pe;
}

QByteArray TaskManager::makePreview(const QString& source, std::uint32_t max_px, const AlphaMode& alpha) {
    const auto t0 = std::chrono::steady_clock::now();
    fprintf(stderr, "[QCutter] makePreview start: %s max_px=%u\n", source.toStdString().c_str(), max_px);
    const fs::path fpath(source.toStdString());

    // 读图像元信息 (用临时 reader)
    ImageInfo info;
    try {
        info = SourceReader::probe(fpath);
    } catch (const std::exception& e) {
        fprintf(stderr, "[QCutter] makePreview probe failed: %s\n", e.what());
        return {};
    }
    const std::uint32_t W = info.width;
    const std::uint32_t H = info.height;
    fprintf(stderr, "[QCutter] makePreview dims: W=%u H=%u\n", W, H);
    if (W == 0 || H == 0) return {};

    // 预览采样参数 (swCutter 风格)
    const std::uint64_t max_dim = std::max<std::uint64_t>(W, H);
    const std::uint32_t scale = static_cast<std::uint32_t>(
        std::max<std::uint64_t>((max_dim + max_px - 1) / max_px, 1ULL));
    const std::uint32_t ow = (W + scale - 1) / scale;
    const std::uint32_t oh = (H + scale - 1) / scale;
    fprintf(stderr, "[QCutter] makePreview scale=%u target=%ux%u (%.2f MB)\n",
            scale, ow, oh, (ow * oh * 4.0) / (1024*1024));
    if (ow == 0 || oh == 0) return {};

    const std::uint32_t cw = info.chunk_w;
    const std::uint32_t ch = info.chunk_h;
    const std::uint32_t chunks_across = (W + cw - 1) / cw;

    // 计算需要解码的 chunk 索引 (swCutter::preview_plan)
    std::vector<std::uint32_t> needed;
    needed.reserve(chunks_across * ((H + ch - 1) / ch));
    for (std::uint32_t ci = 0; ci < info.chunk_count; ++ci) {
        const std::uint32_t cox = (info.chunked_tiles ? (ci % chunks_across) * cw : 0);
        const std::uint32_t coy = (info.chunked_tiles ? (ci / chunks_across) * ch : ci * ch);
        if (cox >= W || coy >= H) continue;
        const std::uint32_t cw_eff = std::min(cw, W - cox);
        const std::uint32_t ch_eff = std::min(ch, H - coy);
        const std::uint32_t ox_lo = (cox + scale - 1) / scale;
        const std::uint32_t oy_lo = (coy + scale - 1) / scale;
        if (ox_lo >= ow || oy_lo >= oh) continue;
        const std::uint32_t ox_end = std::min(ow, ((cox + cw_eff - 1) / scale) + 1);
        const std::uint32_t oy_end = std::min(oh, ((coy + ch_eff - 1) / scale) + 1);
        if (ox_lo < ox_end && oy_lo < oy_end) {
            needed.push_back(ci);
        }
    }
    fprintf(stderr, "[QCutter] makePreview needed=%zu chunks (of %u total)\n",
            needed.size(), info.chunk_count);

    // cells_from_chunk 在 worker 内完成所有像素级操作, 输出 (canvas_offset, RGBA).
    // 不直接写 canvas (主线程独占).
    struct Cell {
        std::uint32_t dst;   // canvas offset (像素数, 实际字节 = dst*4)
        std::uint8_t rgba[4];
    };

    // 多线程解码: 每 worker 独立 SourceReader (避免 libtiff 状态冲突),
    // 通过预览缓存共享解码结果. workers 通过 std::future 同步.
    const unsigned hw = std::max<unsigned>(1, std::thread::hardware_concurrency());
    std::vector<std::future<std::vector<Cell>>> futs;
    const std::size_t chunk_jobs = std::max<std::size_t>(1, needed.size() / (hw * 2));
    for (std::size_t off = 0; off < needed.size(); off += chunk_jobs) {
        const auto end = std::min(needed.size(), off + chunk_jobs);
        futs.push_back(std::async(std::launch::async,
            [&needed, off, end, fpath, W, H, ow, oh, scale, cw, ch, chunks_across]() {
                // 每 worker 新开 SourceReader (libtiff handle 不能跨线程)
                std::vector<Cell> cells;
                std::unique_ptr<SourceReader> reader;
                try {
                    reader = std::make_unique<SourceReader>(fpath);
                } catch (const std::exception& e) {
                    fprintf(stderr, "[QCutter] worker open failed: %s\n", e.what());
                    return cells;
                }
                for (std::size_t i = off; i < end; ++i) {
                    const std::uint32_t ci = needed[i];
                    const bool is_tiled = reader->tiles();
                    const std::uint32_t cox = (is_tiled ? (ci % chunks_across) * cw : 0);
                    const std::uint32_t coy = (is_tiled ? (ci / chunks_across) * ch : ci * ch);
                    const std::uint32_t cw_eff = std::min(cw, W - cox);
                    const std::uint32_t ch_eff = std::min(ch, H - coy);

                    // 取 chunk (优先进程级缓存)
                    preview_cache_internal::ChunkEntry e;
                    bool ok = preview_cache_internal::get_or_insert(fpath, ci,
                        [&]() -> preview_cache_internal::ChunkEntry {
                            preview_cache_internal::ChunkEntry ne;
                            try {
                                auto decoded = reader->decodeChunkPublic(ci);
                                if (!decoded) return ne;
                                ne.rgba = std::move(decoded->rgba);
                                ne.w = decoded->w;
                                ne.h = decoded->h;
                            } catch (const std::exception& ex) {
                                fprintf(stderr, "[QCutter] decodeChunk(%u) failed: %s\n", ci, ex.what());
                            }
                            return ne;
                        },
                        e);
                    if (!ok || e.rgba.empty()) continue;

                    // 采样
                    const std::uint32_t ox_lo = (cox + scale - 1) / scale;
                    const std::uint32_t oy_lo = (coy + scale - 1) / scale;
                    const std::uint32_t ox_end = std::min(ow, ((cox + cw_eff - 1) / scale) + 1);
                    const std::uint32_t oy_end = std::min(oh, ((coy + ch_eff - 1) / scale) + 1);
                    const std::uint32_t actual_w = std::min(e.w, cw_eff);
                    const std::uint32_t actual_h = std::min(e.h, ch_eff);

                    for (std::uint32_t oy = oy_lo; oy < oy_end; ++oy) {
                        const std::uint32_t src_y = oy * scale;
                        if (src_y < coy || src_y >= coy + actual_h) continue;
                        const std::uint32_t lrow = src_y - coy;
                        for (std::uint32_t ox = ox_lo; ox < ox_end; ++ox) {
                            const std::uint32_t src_x = ox * scale;
                            if (src_x >= cox + actual_w) break;
                            const std::uint32_t lcol = src_x - cox;
                            const std::size_t src_off =
                                (static_cast<std::size_t>(lrow) * actual_w + lcol) * 4;
                            if (src_off + 4 > e.rgba.size()) continue;
                            Cell c;
                            c.dst = oy * ow + ox;
                            std::memcpy(c.rgba, e.rgba.data() + src_off, 4);
                            cells.push_back(c);
                        }
                    }
                }
                return cells;
            }));
    }
    // 等待所有 worker, 合并 cells
    std::vector<Cell> all_cells;
    for (auto& f : futs) {
        try {
            auto v = f.get();
            all_cells.insert(all_cells.end(),
                             std::make_move_iterator(v.begin()),
                             std::make_move_iterator(v.end()));
        } catch (const std::exception& e) {
            fprintf(stderr, "[QCutter] worker join failed: %s\n", e.what());
        }
    }
    fprintf(stderr, "[QCutter] makePreview cells=%zu (multi-thread)\n", all_cells.size());

    // 写 canvas (单线程).
    // 用 Qt::transparent 而不是 0 (黑色 alpha=255) —— 缺/未加载像素 alpha=0
    // 透明, 让 PNG 的非图像区在 UI 上透出下方棋盘格, 与黑色图边可清晰分辨.
    QImage canvas(static_cast<int>(ow), static_cast<int>(oh), QImage::Format_RGBA8888);
    canvas.fill(Qt::transparent);
    for (const auto& c : all_cells) {
        const std::size_t off = static_cast<std::size_t>(c.dst) * 4;
        if (off + 4 <= canvas.sizeInBytes()) {
            std::memcpy(canvas.bits() + off, c.rgba, 4);
        }
    }
    fprintf(stderr, "[QCutter] makePreview canvas filled, encoding PNG\n");
    auto* bits = canvas.bits();
    const auto n = static_cast<std::size_t>(canvas.width()) * canvas.height();
    alpha.apply(bits, n);
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    canvas.save(&buf, "PNG");
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    fprintf(stderr, "[QCutter] makePreview done, %lld bytes, %lld ms\n", (long long)ba.size(), (long long)ms);
    return ba;
}

std::vector<std::uint8_t> TaskManager::samplePixel(const QString& source, std::int64_t x, std::int64_t y) {
    SourceReader r(fs::path(source.toStdString()));
    if (x < 0 || y < 0 || x >= r.width() || y >= r.height()) {
        throw CoreError::invalid("sample out of range");
    }
    return r.readRect(x, y, 1, 1);
}

std::uint64_t TaskManager::startTask(const TaskConfig& cfg) {
    if (!fs::is_regular_file(fs::path(cfg.source.toStdString()))) {
        throw CoreError::invalid("source not found: " + cfg.source.toStdString());
    }
    auto e = std::make_shared<Impl::Entry>();
    e->id = d_->next_id++;
    e->cfg = cfg;
    e->status = "queued";
    e->started_ms = 0;
    e->finished_ms = 0;
    {
        std::lock_guard<std::mutex> lk(d_->mgr_mu);
        d_->entries.push_back(e);
    }
    d_->persistAll();
    d_->broadcast(TaskEvent::statusChanged(e->id, e->status));
    // 启动 worker 线程
    std::thread([this, e]() { d_->worker(e); }).detach();
    return e->id;
}

bool TaskManager::cancelTask(std::uint64_t id) {
    auto e = d_->findById(id);
    if (!e) return false;
    e->cancel.store(true);
    e->control->cancel.store(true);
    return true;
}

bool TaskManager::pauseTask(std::uint64_t id) {
    auto e = d_->findById(id);
    if (!e || e->status != "running") return false;
    e->control->paused.store(true);
    e->status = "paused";
    d_->persistAll();
    d_->broadcast(TaskEvent::statusChanged(e->id, e->status));
    return true;
}

bool TaskManager::resumeTask(std::uint64_t id) {
    auto e = d_->findById(id);
    if (!e || e->status != "paused") return false;
    e->control->paused.store(false);
    e->status = "running";
    d_->persistAll();
    d_->broadcast(TaskEvent::statusChanged(e->id, e->status));
    return true;
}

bool TaskManager::removeTask(std::uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(d_->mgr_mu);
        auto it = std::find_if(d_->entries.begin(), d_->entries.end(),
                               [&](const std::shared_ptr<Impl::Entry>& x) { return x->id == id; });
        if (it == d_->entries.end()) return false;
        if ((*it)->status == "running") return false;
        d_->entries.erase(it);
    }
    d_->persistAll();
    // 通知 UI 拉取最新列表, 否则 AppState 缓存里仍保留旧 task
    d_->broadcast(TaskEvent::removed(id));
    return true;
}

void TaskManager::setMaxConcurrency(std::uint32_t n) {
    n = std::clamp<std::uint32_t>(n, 1, 16);
    std::lock_guard<std::mutex> lk(d_->mgr_mu);
    d_->max_concurrency = n;
    d_->mgr_cv.notify_all();
}
std::uint32_t TaskManager::maxConcurrency() const {
    std::lock_guard<std::mutex> lk(d_->mgr_mu);
    return d_->max_concurrency;
}

std::vector<TaskDto> TaskManager::listTasks() const {
    std::vector<TaskDto> out;
    std::lock_guard<std::mutex> lk(d_->mgr_mu);
    for (const auto& e : d_->entries) {
        TaskDto t;
        t.id = e->id;
        t.source = e->cfg.source;
        t.output = e->cfg.output;
        t.tile_size = e->cfg.tile_size;
        t.scheme = e->cfg.scheme;
        t.alpha = e->cfg.alpha;
        t.resample = e->cfg.resample;
        t.zmin = e->cfg.zmin;
        t.zmax = e->cfg.zmax;
        t.status = e->status;
        t.level = e->level;
        t.tiles_done = e->tiles_done;
        t.total_tiles = e->total_tiles;
        t.bytes_written = e->bytes_written;
        if (e->status == "running") {
            const auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - e->started_at).count();
            t.elapsed_ms = static_cast<std::uint64_t>(dur);
        } else {
            t.elapsed_ms = e->elapsed_ms;
        }
        t.error = e->error;
        t.started_at_ms = e->started_ms;
        t.finished_at_ms = e->finished_ms;
        out.push_back(std::move(t));
    }
    return out;
}

} // namespace qcutter