// 单任务卡片: 进度条 + 操作按钮.
#pragma once
#include "api/task_manager.h"
#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <memory>

namespace qcutter {

class AppState;

class TaskCard : public QWidget {
    Q_OBJECT
public:
    TaskCard(std::shared_ptr<AppState> state, TaskDto dto, QWidget* parent = nullptr);

    void updateDto(const TaskDto& dto);

    TaskDto dto() const { return dto_; }

    QString status() const { return dto_.status; }
    quint64 id() const { return dto_.id; }

signals:
    void cancelClicked(quint64 id);
    void pauseClicked(quint64 id);
    void resumeClicked(quint64 id);
    void removeClicked(quint64 id);
    void openOutputClicked(const QString& path);
    void previewClicked(const QString& path);

private:
    void refresh();
    QString fmtBytes(quint64 n) const;
    QString fmtSpeed(int bps) const;
    QString fmtDuration(quint64 ms) const;

    std::shared_ptr<AppState> state_;
    TaskDto dto_;
    QLabel* title_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* detail_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;
    QPushButton* removeBtn_ = nullptr;
    QPushButton* openBtn_ = nullptr;
    QPushButton* previewBtn_ = nullptr;
};

} // namespace qcutter