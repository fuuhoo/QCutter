#include "ui/pages/tasks_page.h"
#include "state/app_state.h"
#include "api/task_manager.h"
#include "api/preview_server.h"
#include "ui/widgets/task_card.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QTimer>
#include <QProcess>

namespace qcutter {

TasksPage::TasksPage(std::shared_ptr<AppState> state, QWidget* parent)
    : QWidget(parent), state_(std::move(state)) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(8);

    auto* header = new QLabel("任务中心");
    QFont hf; hf.setPointSize(13); hf.setBold(true);
    header->setFont(hf);
    root->addWidget(header);

    auto* tip = new QLabel("支持启动/暂停/取消/删除任务。点击「预览」在浏览器打开 MapLibre 切片预览。");
    tip->setStyleSheet("color: #7a8699; font-size: 11px");
    root->addWidget(tip);

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    cardsHost_ = new QWidget();
    auto* cl = new QVBoxLayout(cardsHost_);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(8);
    cl->addStretch();
    scroll->setWidget(cardsHost_);
    root->addWidget(scroll, 1);

    connect(state_.get(), &AppState::changed, this, &TasksPage::rebuild);
    rebuild();
}

void TasksPage::rebuild() {
    if (!cardsHost_) return;
    auto* cl = qobject_cast<QVBoxLayout*>(cardsHost_->layout());
    if (!cl) return;
    // 移除所有卡片
    for (auto* c : cards_) {
        cl->removeWidget(c);
        c->deleteLater();
    }
    cards_.clear();
    const auto tasks = state_->tasks();
    for (const auto& t : tasks) {
        auto* c = new TaskCard(state_, t, cardsHost_);
        cl->addWidget(c);
        connect(c, &TaskCard::cancelClicked, this, [this](quint64 id) {
            TaskManager::instance().cancelTask(id);
        });
        connect(c, &TaskCard::pauseClicked, this, [this](quint64 id) {
            TaskManager::instance().pauseTask(id);
        });
        connect(c, &TaskCard::resumeClicked, this, [this](quint64 id) {
            TaskManager::instance().resumeTask(id);
        });
        connect(c, &TaskCard::removeClicked, this, [this](quint64 id) {
            TaskManager::instance().removeTask(id);
        });
        connect(c, &TaskCard::openOutputClicked, this, [](const QString& path) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        connect(c, &TaskCard::previewClicked, this, [this](const QString& path) {
            try {
                auto port = PreviewServer::serve(path.toStdString());
                QDesktopServices::openUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/preview.html").arg(port)));
            } catch (const std::exception& e) {
                QMessageBox::warning(this, "预览失败", QString::fromUtf8(e.what()));
            }
        });
        cards_.append(c);
    }
}

} // namespace qcutter