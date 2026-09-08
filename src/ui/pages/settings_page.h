// 设置页: 全局参数/底图/地图密钥/主题.
#pragma once
#include <QWidget>
#include <memory>

namespace qcutter {

class AppState;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(std::shared_ptr<AppState> state, QWidget* parent = nullptr);

private:
    std::shared_ptr<AppState> state_;
};

} // namespace qcutter