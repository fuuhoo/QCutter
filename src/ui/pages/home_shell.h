// 左侧导航 + 右侧内容: 新建任务 / 任务中心 / 设置.
#pragma once
#include <QWidget>
#include <memory>
#include <QStackedWidget>

namespace qcutter {

class AppState;
class NewTaskPage;
class TasksPage;
class SettingsPage;

class HomeShell : public QWidget {
    Q_OBJECT
public:
    explicit HomeShell(std::shared_ptr<AppState> state, QWidget* parent = nullptr);

private slots:
    void selectPage(int index);

private:
    std::shared_ptr<AppState> state_;
    QStackedWidget* stack_ = nullptr;
};

} // namespace qcutter