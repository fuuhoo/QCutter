#include "state/app_state.h"
#include "util/paths.h"
#include "util/logger.h"
#include <QFile>
#include <QDir>
#include <QVector>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QHash>
#include <QStandardPaths>
#include <algorithm>
#include <vector>

namespace qcutter {

static fs::path settingsPath() {
    auto d = appDataDir();
    fs::create_directories(d);
    return d / "settings.json";
}

AppState::AppState(QObject* parent) : QObject(parent) {
    connect(&TaskManager::instance(), &TaskManager::eventReady,
            this, &AppState::onEvent);
}

AppState::~AppState() = default;

void AppState::loadSettings() {
    const auto p = settingsPath();
    QFile f(QString::fromStdString(p.string()));
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        const auto doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            const auto j = doc.object();
            concurrency_   = j.value("concurrency").toInt(2);
            defaultOutput_ = j.value("defaultOutput").toString();
            darkTheme_     = j.value("themeMode").toInt(1) != 0; // 0=light, others=dark
            tileSize_      = j.value("tileSize").toInt(256);
            skipEmpty_     = j.value("skipEmpty").toBool(false);
            resample_      = j.value("resample").toInt(1) == 0 ? Resample::Nearest : Resample::Bilinear;
            const auto mk = j.value("mapKeys");
            if (mk.isObject()) {
                mapKeys_.clear();
                for (auto it = mk.toObject().begin(); it != mk.toObject().end(); ++it) {
                    mapKeys_ << it.key() + "=" + it.value().toString();
                }
            }
            const auto bm = j.value("baseMaps");
            if (bm.isArray()) {
                // 兼容历史数据: 读出来后只保留 key 在内置预设表里的项, 并把
                // 用户态字段 (on/below/tms/opacity) 同步进 baseMaps_. 真正的
                // URL 模板始终来自预设, 不受持久化数据影响.
                for (const auto& v : bm.toArray().toVariantList()) {
                    const auto m = v.toMap();
                    const auto k = m.value("key").toString();
                    if (k.isEmpty()) continue;
                    // 找到同 key 的预设
                    bool found = false;
                    for (const auto& p2 : basemapPresets()) {
                        if (p2.key == k) { found = true; break; }
                    }
                    if (!found) continue;
                    userState_[k] = m;
                }
            }
        }
    }
    // 始终用内置预设初始化底图列表. 用户不能新增/删除/改 URL; 只可改
    // on / below / tms / opacity 四个开关.
    baseMaps_.clear();
    for (const auto& preset : basemapPresets()) {
        QVariantMap m;
        m["key"]     = preset.key;
        m["name"]    = preset.name;
        m["tpl"]     = preset.tpl;
        m["subs"]    = preset.subs;
        m["tms"]     = preset.tms;
        m["below"]   = preset.below;
        m["opacity"] = preset.opacity;
        m["zmin"]    = preset.zmin;
        m["zmax"]    = preset.zmax;
        // 默认: OSM 启用, 其它关闭; 持久化里若用户改过则用之.
        bool on = (preset.key == "osm");
        if (userState_.contains(preset.key)) {
            const auto& u = userState_[preset.key];
            on = u.value("on", on).toBool();
            m["tms"]     = u.value("tms", preset.tms).toBool();
            m["below"]   = u.value("below", preset.below).toBool();
            m["opacity"] = u.value("opacity", preset.opacity).toDouble();
        }
        m["on"] = on;
        baseMaps_.append(m);
    }
    if (mapKeys_.isEmpty()) mapKeys_ << "tk=";
}

void AppState::saveSettings() {
    QJsonObject o;
    o["concurrency"] = concurrency_;
    o["defaultOutput"] = defaultOutput_;
    o["themeMode"] = darkTheme_ ? 1 : 0;
    o["tileSize"] = tileSize_;
    o["skipEmpty"] = skipEmpty_;
    o["resample"] = resample_ == Resample::Nearest ? 0 : 1;
    QJsonObject mk;
    for (const auto& kv : mapKeys_) {
        const auto eq = kv.indexOf('=');
        if (eq <= 0) continue;
        mk.insert(kv.left(eq), kv.mid(eq + 1));
    }
    o["mapKeys"] = mk;
    o["baseMaps"] = QJsonArray::fromVariantList(baseMaps_);

    const auto p = settingsPath();
    QFile f(QString::fromStdString(p.string()));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    }
}

void AppState::bootstrap() {
    loadSettings();
    try {
        TaskManager::instance().setMaxConcurrency(static_cast<std::uint32_t>(concurrency_));
    } catch (...) {}
    refreshTasks();
    ticker_.setInterval(1000);
    connect(&ticker_, &QTimer::timeout, this, &AppState::onTick);
    ticker_.start();
    emit changed();
}

void AppState::refreshTasks() {
    tasks_ = QVector<TaskDto>::fromStdVector(TaskManager::instance().listTasks()).toList();
    emit changed();
}

void AppState::onEvent(TaskEvent ev) {
    if (ev.kind == TaskEvent::Kind::Removed) {
        // 任务被删除: 重新拉取列表. 比在缓存里逐项 splice 更安全 (其他线程可能
        // 也在改 entries_, 比如新的 startTask). refreshTasks 会从 TaskManager
        // 拉一份新快照并 emit changed(), 触发 TasksPage::rebuild 重建 UI.
        refreshTasks();
        return;
    }
    auto it = std::find_if(tasks_.begin(), tasks_.end(),
                           [&](const TaskDto& t) { return t.id == ev.task_id; });
    if (it == tasks_.end()) { refreshTasks(); return; }
    auto& t = *it;
    switch (ev.kind) {
    case TaskEvent::Kind::StatusChanged:
        t.status = ev.status;
        break;
    case TaskEvent::Kind::Started:
        t.status = "running";
        t.total_tiles = ev.total_tiles;
        t.started_at_ms = QDateTime::currentMSecsSinceEpoch();
        break;
    case TaskEvent::Kind::LevelStart:
        t.level = ev.level;
        break;
    case TaskEvent::Kind::Progress:
        t.level = ev.level;
        t.tiles_done = ev.tiles_done;
        t.total_tiles = ev.total_tiles;
        t.bytes_written = ev.bytes_written;
        t.elapsed_ms = ev.elapsed_ms;
        break;
    case TaskEvent::Kind::Finished: {
        t.tiles_done = ev.summary.tiles_done;
        t.total_tiles = ev.summary.total_tiles;
        t.bytes_written = ev.summary.bytes_written;
        t.elapsed_ms = ev.summary.elapsed_ms;
        if (ev.summary.error) t.error = *ev.summary.error;
        t.finished_at_ms = QDateTime::currentMSecsSinceEpoch();
        t.status = ev.summary.cancelled ? "cancelled" : (t.error ? "error" : "done");
        speedBps_.remove(static_cast<int>(ev.task_id));
        break;
    }
    }
    emit changed();
}

void AppState::onTick() {
    const auto now = QDateTime::currentDateTime();
    QHash<int, QPair<int, QDateTime>> newPrev;
    for (const auto& t : tasks_) {
        const int id = static_cast<int>(t.id);
        newPrev[id] = { static_cast<int>(t.bytes_written), now };
        const auto p = prev_.value(id);
        if (p.first == 0 && p.second.isNull()) continue;
        const auto dt = p.second.msecsTo(now);
        if (dt > 400 && t.status == "running") {
            const qint64 delta = static_cast<qint64>(t.bytes_written) - p.first;
            speedBps_[id] = static_cast<int>(delta * 1000 / dt);
        } else if (t.status != "running") {
            speedBps_.remove(id);
        }
    }
    prev_ = newPrev;
    emit changed();
}

QList<TaskDto> AppState::tasks() const { return tasks_; }

int AppState::speedBps(int taskId) const {
    return speedBps_.value(taskId, 0);
}

QString AppState::overlaysJson() const {
    // 收集所有 mapKeys (如 tk=xxx) 以便在 URL 模板里替换 {tk} 等占位符.
    // 桌面端的 settings_page 就是这套机制: tk 单独存在 mapKeys_ 里, URL 模板只
    // 写 {tk} 占位符. preview.html 的 JS 也按这套替换, 但需要把密钥当作 overlay
    // 的字段读出. 在此把 mapKeys 合并到每条 overlay, JS 替换代码 o[k] 就能命中.
    QJsonObject mapKeyObj;
    for (const auto& kv : mapKeys_) {
        const auto eq = kv.indexOf('=');
        if (eq <= 0) continue;
        mapKeyObj.insert(kv.left(eq), kv.mid(eq + 1));
    }
    // 子域字符串: 之前用空格分隔 (例如 "0 1 2 3 4 5 6 7"). 在 URL 模板里
    // 用 {s} 占位符会被 JS 当作子域替换循环; 但当前预览 JS 走的 "逐字符 split"
    // 会把空格当子域, 导致 "t .tianditu.gov.cn". 我们把 subs 字段以单字符串
    // 传, 浏览器按数组逐个尝试; 子域可以是 "0"/"1"/.../"7" 这 8 个值, 不能含空格.
    QJsonArray arr;
    for (const auto& v : baseMaps_) {
        QVariantMap m = v.toMap();
        // 规范化 subs: 去掉所有空白, 只保留非空字符; 若为空 (OSM), 用 [""]
        const QString subsRaw = m.value("subs").toString();
        QStringList subsList;
        for (const QChar c : subsRaw) {
            if (!c.isSpace() && c != QChar(',')) subsList << QString(c);
        }
        if (subsList.isEmpty()) subsList << QString();
        m["subs"] = subsList; // 转成 QStringList (即 JSON 数组)
        // 把 mapKeys 合并进 overlay
        for (auto it = mapKeyObj.begin(); it != mapKeyObj.end(); ++it) {
            m.insert(it.key(), QVariant(it.value().toString()));
        }
        arr.append(QJsonObject::fromVariantMap(m));
    }
    QJsonObject o;
    o["overlays"] = arr;
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void AppState::setConcurrency(int n) {
    n = std::clamp(n, 1, 16);
    concurrency_ = n;
    try { TaskManager::instance().setMaxConcurrency(static_cast<std::uint32_t>(n)); }
    catch (...) {}
    saveSettings();
    emit changed();
}
void AppState::setDefaultOutput(const QString& p) { defaultOutput_ = p; saveSettings(); emit changed(); }
void AppState::setTileSize(int v) {
    v = std::clamp(v, 64, 1024);
    if ((v & (v - 1)) != 0) return;
    tileSize_ = v;
    saveSettings();
    emit changed();
}
void AppState::setSkipEmpty(bool v) { skipEmpty_ = v; saveSettings(); emit changed(); }
void AppState::setResample(Resample r) { resample_ = r; saveSettings(); emit changed(); }
void AppState::setDarkTheme(bool v) { darkTheme_ = v; saveSettings(); emit changed(); }

QStringList AppState::mapKeys() const { return mapKeys_; }

void AppState::setMapKey(const QString& name, const QString& value) {
    for (auto& kv : mapKeys_) {
        if (kv.startsWith(name + "=")) {
            kv = name + "=" + value;
            saveSettings();
            emit changed();
            return;
        }
    }
    mapKeys_ << name + "=" + value;
    saveSettings();
    emit changed();
}

void AppState::addBasemap(const QVariantMap& /*layer*/) {
    // 不再支持用户新增底图. 底图列表固定为内置 OSM / 天地图等预设.
}
void AppState::removeBasemap(int /*i*/) {
    // 不再支持用户删除底图.
}

const std::vector<BasemapPreset>& AppState::basemapPresets() {
    static const std::vector<BasemapPreset> kPresets = {
        {
            "osm",
            "OpenStreetMap",
            "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
            "",
            false,
            true,
            1.0,
            0,
            19,
        },
        {
            "tdt_img",
            "天地图 影像",
            "https://t{s}.tianditu.gov.cn/img_w/wmts"
            "?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0"
            "&LAYER=img&STYLE=default&TILEMATRIXSET=w"
            "&FORMAT=tiles&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}&tk={tk}",
            "0 1 2 3 4 5 6 7",
            false,
            true,
            1.0,
            1,
            18,
        },
    };
    return kPresets;
}

QVariantMap AppState::basemapByKey(const QString& key) const {
    for (const auto& v : baseMaps_) {
        const auto m = v.toMap();
        if (m.value("key").toString() == key) return m;
    }
    return {};
}

void AppState::setBasemapOn(const QString& key, bool on) {
    for (int i = 0; i < baseMaps_.size(); ++i) {
        QVariantMap m = baseMaps_[i].toMap();
        if (m.value("key").toString() != key) continue;
        m["on"] = on;
        baseMaps_[i] = m;
        saveSettings();
        emit changed();
        return;
    }
}
void AppState::setBasemapBelow(const QString& key, bool below) {
    for (int i = 0; i < baseMaps_.size(); ++i) {
        QVariantMap m = baseMaps_[i].toMap();
        if (m.value("key").toString() != key) continue;
        m["below"] = below;
        baseMaps_[i] = m;
        saveSettings();
        emit changed();
        return;
    }
}
void AppState::setBasemapTms(const QString& key, bool tms) {
    for (int i = 0; i < baseMaps_.size(); ++i) {
        QVariantMap m = baseMaps_[i].toMap();
        if (m.value("key").toString() != key) continue;
        m["tms"] = tms;
        baseMaps_[i] = m;
        saveSettings();
        emit changed();
        return;
    }
}

void AppState::setSettingsDirty() { settingsDirty_ = true; }

} // namespace qcutter