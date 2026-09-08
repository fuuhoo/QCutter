// 任务中心: 任务列表 + 进度条 + 操作按钮.
#pragma once
#include <QWidget>
#include <memory>
#include <QList>

namespace qcutter {

class AppState;
class TaskCard;
struct TaskDto;

class TasksPage : public QWidget {
    Q_OBJECT
public:
    explicit TasksPage(std::shared_ptr<AppState> state, QWidget* parent = nullptr);

private:
    void rebuild();
    void refreshSpeeds();

    std::shared_ptr<AppState> state_;
    QWidget* cardsHost_ = nullptr;
    QList<TaskCard*> cards_;
};

} // namespace qcutter