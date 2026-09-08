// 全局状态: 任务列表 / 设置 / 速度计算.
#pragma once
#include "api/task_manager.h"
#include <QObject>
#include <QTimer>
#include <QHash>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <vector>

class QDateTime;

namespace qcutter {

/// 内置底图定义. 硬编码的底图, 用户只能切显隐/层级/TMS, 不能新增/删除/改 URL.
/// 字段: id/key, name, tpl(URL 模板, 占位符 {s}=子域 {z}{x}{y}=瓦片坐标, tk=密钥),
///       subs(子域列表, 空格分隔), tms(默认 scheme), below(默认放在切片下).
struct BasemapPreset {
    QString key;       // 内部 id, 例如 "osm", "tdt_vec", "tdt_img"
    QString name;      // 显示名
    QString tpl;       // URL 模板
    QString subs;      // 子域字母, 空格分隔
    bool    tms;       // 默认 scheme
    bool    below;     // 默认放在切片下
    double  opacity;   // 默认透明度
    int     zmin;
    int     zmax;
};

class AppState : public QObject {
    Q_OBJECT
public:
    explicit AppState(QObject* parent = nullptr);
    ~AppState();

    /// 加载设置 (主题/默认输出/瓦片尺寸/skip_empty/resample/底图列表).
    void loadSettings();
    void saveSettings();

    /// 启动: 拉历史 / 订阅事件 / 启动速度 tick.
    void bootstrap();

    // --- 设置 ---
    int concurrency() const { return concurrency_; }
    void setConcurrency(int n);
    QString defaultOutput() const { return defaultOutput_; }
    void setDefaultOutput(const QString& p);
    int tileSize() const { return tileSize_; }
    void setTileSize(int v);
    bool skipEmpty() const { return skipEmpty_; }
    void setSkipEmpty(bool v);
    Resample resample() const { return resample_; }
    void setResample(Resample r);
    bool darkTheme() const { return darkTheme_; }
    void setDarkTheme(bool v);
    QStringList mapKeys() const;
    QVariantList baseMapks() const { return baseMaps_; }
    /// 内置底图预设 (OSM / 天地图 等), 顺序固定.
    static const std::vector<BasemapPreset>& basemapPresets();
    /// 根据 preset.key 查 QVariantMap(包含用户 on/off/below/tms/opacity/...);
    /// 若 key 不存在返回空 map.
    QVariantMap basemapByKey(const QString& key) const;
    void addBasemap(const QVariantMap& layer);
    void removeBasemap(int index);
    void setBasemapOn(const QString& key, bool on);
    void setBasemapBelow(const QString& key, bool below);
    void setBasemapTms(const QString& key, bool tms);
    void setMapKey(const QString& name, const QString& value);

    /// 任务列表 (按 id 升序).
    QList<TaskDto> tasks() const;

    /// 速度 (bytes/sec), 仅 running 任务有值.
    int speedBps(int taskId) const;

    /// 全局预览 overlays (JSON 字符串).
    QString overlaysJson() const;

signals:
    void changed();

private slots:
    void onEvent(TaskEvent ev);
    void onTick();

private:
    void refreshTasks();
    void setSettingsDirty();

    int concurrency_ = 2;
    QString defaultOutput_;
    int tileSize_ = 256;
    bool skipEmpty_ = false;
    Resample resample_ = Resample::Bilinear;
    bool darkTheme_ = true;
    QStringList mapKeys_;
    /// 持久化: 每项含 key, name, tpl, subs, tms, on, below, opacity, zmin, zmax
    QVariantList baseMaps_;
    /// 加载设置时临时存的用户态 (on/tms/below/opacity), 用于在不污染 baseMaps_
    /// 默认值的前提下合并. 加载完成后即丢弃.
    QHash<QString, QVariantMap> userState_;

    QList<TaskDto> tasks_;
    QHash<int, int> speedBps_;
    QHash<int, QPair<int, QDateTime>> prev_;

    QTimer ticker_;
    bool settingsDirty_ = false;
};

} // namespace qcutter