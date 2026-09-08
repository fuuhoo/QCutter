#include "api/history_store.h"
#include "util/paths.h"
#include "util/logger.h"
#include <sqlite3.h>
#include <experimental/filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace qcutter {

namespace fs = std::experimental::filesystem;

HistoryStore& HistoryStore::instance() {
    static HistoryStore h;
    return h;
}

static std::string dbPath() {
    auto d = appDataDir();
    fs::create_directories(d);
    return (d / "history.db").string();
}

static QString schemeToStr(Scheme s) {
    return s == Scheme::Xyz ? "xyz" : "tms";
}
static Scheme schemeFromQ(const QString& s) {
    return s == "tms" ? Scheme::Tms : Scheme::Xyz;
}

void HistoryStore::init() {
    std::lock_guard<std::mutex> lk(mu_);
    const auto path = dbPath();
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        LOG_E("history_store", QStringLiteral("open db failed: %1").arg(sqlite3_errmsg(db)));
        if (db) sqlite3_close(db);
        return;
    }
    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id INTEGER PRIMARY KEY,"
        "  source TEXT NOT NULL,"
        "  output TEXT NOT NULL,"
        "  tile_size INTEGER NOT NULL,"
        "  scheme TEXT NOT NULL,"
        "  alpha TEXT NOT NULL,"
        "  resample TEXT NOT NULL,"
        "  zmin INTEGER,"
        "  zmax INTEGER,"
        "  skip_empty INTEGER NOT NULL DEFAULT 0,"
        "  mercator INTEGER NOT NULL DEFAULT 0,"
        "  precise INTEGER NOT NULL DEFAULT 0,"
        "  status TEXT NOT NULL,"
        "  level INTEGER NOT NULL DEFAULT 0,"
        "  tiles_done INTEGER NOT NULL DEFAULT 0,"
        "  total_tiles INTEGER NOT NULL DEFAULT 0,"
        "  bytes_written INTEGER NOT NULL DEFAULT 0,"
        "  elapsed_ms INTEGER NOT NULL DEFAULT 0,"
        "  error TEXT,"
        "  started_ms INTEGER NOT NULL DEFAULT 0,"
        "  finished_ms INTEGER NOT NULL DEFAULT 0,"
        "  bounds_json TEXT"
        ");";
    char* errmsg = nullptr;
    if (sqlite3_exec(db, schema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        LOG_E("history_store", QStringLiteral("schema init: %1").arg(errmsg));
        sqlite3_free(errmsg);
    }
    // 旧库迁移
    sqlite3_exec(db, "ALTER TABLE tasks ADD COLUMN skip_empty INTEGER NOT NULL DEFAULT 0", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "ALTER TABLE tasks ADD COLUMN mercator INTEGER NOT NULL DEFAULT 0", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "ALTER TABLE tasks ADD COLUMN precise INTEGER NOT NULL DEFAULT 0", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "ALTER TABLE tasks ADD COLUMN bounds_json TEXT", nullptr, nullptr, nullptr);

    sqlite3_close(db);
}

static QString ser(Scheme s) { return schemeToStr(s); }
static QString ser(Resample r) { return r == Resample::Nearest ? "nearest" : "bilinear"; }
static QString ser(const AlphaMode& a) { return QString::fromStdString(a.toJson().dump()); }
static Scheme schemeDe(const QString& s) { return schemeFromQ(s); }
static Resample resampleDe(const QString& s) { return s == "nearest" ? Resample::Nearest : Resample::Bilinear; }
static AlphaMode alphaDe(const QString& s) {
    try { return AlphaMode::fromJson(nlohmann::json::parse(s.toStdString())); }
    catch (...) { return AlphaMode::keep(); }
}

std::vector<HistoryRec> HistoryStore::load() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<HistoryRec> out;
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath().c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    const char* sql =
        "SELECT id, source, output, tile_size, scheme, alpha, resample, zmin, zmax,"
        " skip_empty, mercator, precise,"
        " status, level, tiles_done, total_tiles, bytes_written, elapsed_ms,"
        " error, started_ms, finished_ms, bounds_json"
        " FROM tasks ORDER BY id ASC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return out;
    }
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) break;
        HistoryRec r;
        r.id = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        r.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.output = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        r.tile_size = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 3));
        r.scheme = schemeDe(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        r.alpha = alphaDe(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        r.resample = resampleDe(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)));
        if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) r.zmin = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 7));
        if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) r.zmax = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 8));
        r.skip_empty = sqlite3_column_int(stmt, 9) != 0;
        r.mercator = sqlite3_column_int(stmt, 10) != 0;
        // precise 旧库兼容: 默认 true
        r.precise = sqlite3_column_type(stmt, 11) == SQLITE_NULL
                  ? true : (sqlite3_column_int(stmt, 11) != 0);
        r.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        r.level = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 13));
        r.tiles_done = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 14));
        r.total_tiles = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 15));
        r.bytes_written = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 16));
        r.elapsed_ms = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 17));
        if (sqlite3_column_type(stmt, 18) != SQLITE_NULL)
            r.error = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 18)));
        r.started_ms = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 19));
        r.finished_ms = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 20));
        if (sqlite3_column_type(stmt, 21) != SQLITE_NULL)
            r.bounds_json = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 21)));
        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

void HistoryStore::save(const std::vector<HistoryRec>& recs) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath().c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DELETE FROM tasks", nullptr, nullptr, nullptr);
    const char* ins =
        "INSERT INTO tasks (id, source, output, tile_size, scheme, alpha, resample,"
        " zmin, zmax, skip_empty, mercator, precise,"
        " status, level, tiles_done, total_tiles, bytes_written, elapsed_ms,"
        " error, started_ms, finished_ms, bounds_json)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, ins, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        return;
    }
    for (const auto& r : recs) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(r.id));
        sqlite3_bind_text(stmt, 2, r.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, r.output.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, static_cast<int>(r.tile_size));
        sqlite3_bind_text(stmt, 5, ser(r.scheme).toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, ser(r.alpha).toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, ser(r.resample).toUtf8().constData(), -1, SQLITE_TRANSIENT);
        if (r.zmin) sqlite3_bind_int(stmt, 8, static_cast<int>(*r.zmin)); else sqlite3_bind_null(stmt, 8);
        if (r.zmax) sqlite3_bind_int(stmt, 9, static_cast<int>(*r.zmax)); else sqlite3_bind_null(stmt, 9);
        sqlite3_bind_int(stmt, 10, r.skip_empty ? 1 : 0);
        sqlite3_bind_int(stmt, 11, r.mercator ? 1 : 0);
        sqlite3_bind_int(stmt, 12, r.precise ? 1 : 0);
        sqlite3_bind_text(stmt, 13, r.status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 14, static_cast<int>(r.level));
        sqlite3_bind_int64(stmt, 15, static_cast<sqlite3_int64>(r.tiles_done));
        sqlite3_bind_int64(stmt, 16, static_cast<sqlite3_int64>(r.total_tiles));
        sqlite3_bind_int64(stmt, 17, static_cast<sqlite3_int64>(r.bytes_written));
        sqlite3_bind_int64(stmt, 18, static_cast<sqlite3_int64>(r.elapsed_ms));
        if (r.error) sqlite3_bind_text(stmt, 19, r.error->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 19);
        sqlite3_bind_int64(stmt, 20, static_cast<sqlite3_int64>(r.started_ms));
        sqlite3_bind_int64(stmt, 21, static_cast<sqlite3_int64>(r.finished_ms));
        if (r.bounds_json) sqlite3_bind_text(stmt, 22, r.bounds_json->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 22);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            LOG_E("history_store", QStringLiteral("insert failed: %1")
                                       .arg(reinterpret_cast<const char*>(sqlite3_errmsg(db))));
            break;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

} // namespace qcutter