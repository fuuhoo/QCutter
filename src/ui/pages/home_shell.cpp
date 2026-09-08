#include "ui/pages/home_shell.h"
#include "state/app_state.h"
#include "ui/pages/new_task_page.h"
#include "ui/pages/tasks_page.h"
#include "ui/pages/settings_page.h"
#include "ui/app_theme.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QListWidget>
#include <QLabel>
#include <QStackedWidget>
#include <QPushButton>
#include <QIcon>
#include <QFont>

namespace qcutter {

HomeShell::HomeShell(std::shared_ptr<AppState> state, QWidget* parent)
    : QWidget(parent), state_(std::move(state)) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 左侧导航
    auto* sidebar = new QWidget();
    sidebar->setFixedWidth(180);
    sidebar->setStyleSheet(QStringLiteral("background-color: %1;")
                           .arg(AppTheme::panelColor(state_->darkTheme()).name()));
    auto* sb_layout = new QVBoxLayout(sidebar);
    sb_layout->setContentsMargins(12, 20, 12, 20);
    sb_layout->setSpacing(6);

    auto* title = new QLabel("QCutter");
    QFont tf; tf.setPointSize(14); tf.setBold(true);
    title->setFont(tf);
    sb_layout->addWidget(title);

    auto* sub = new QLabel("TIFF 金字塔切片");
    sub->setStyleSheet("color: #7a8699; font-size: 11px");
    sb_layout->addWidget(sub);

    sb_layout->addSpacing(20);

    auto* nav = new QListWidget();
    nav->setStyleSheet(QString(
        "QListWidget { background: transparent; border: none; padding: 0; }"
        "QListWidget::item { padding: 10px 12px; border-radius: 6px; }"
        "QListWidget::item:selected { background: %1; color: white; }"
    ).arg(AppTheme::accentColor().name()));
    nav->addItem("📄 新建任务");
    nav->addItem("📋 任务中心");
    nav->addItem("⚙️ 设置");
    nav->setCurrentRow(0);
    nav->setFocusPolicy(Qt::NoFocus);
    connect(nav, &QListWidget::currentRowChanged, this, &HomeShell::selectPage);
    sb_layout->addWidget(nav);

    sb_layout->addStretch();

    auto* ver = new QLabel("v0.1.0");
    ver->setStyleSheet("color: #7a8699; font-size: 10px");
    sb_layout->addWidget(ver);

    layout->addWidget(sidebar);

    // 右侧内容
    stack_ = new QStackedWidget();
    stack_->addWidget(new NewTaskPage(state_));
    stack_->addWidget(new TasksPage(state_));
    stack_->addWidget(new SettingsPage(state_));
    layout->addWidget(stack_, 1);
}

void HomeShell::selectPage(int index) {
    if (stack_) stack_->setCurrentIndex(index);
}

} // namespace qcutter