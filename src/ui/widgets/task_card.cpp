#include "ui/widgets/task_card.h"
#include "state/app_state.h"
#include "ui/app_theme.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QFont>
#include <QColor>
#include <QFileInfo>

namespace qcutter {

QString TaskCard::fmtBytes(quint64 n) const {
    if (n < 1024) return QString::number(n) + " B";
    if (n < 1024ULL * 1024) return QString::number(n / 1024.0, 'f', 1) + " KB";
    if (n < 1024ULL * 1024 * 1024) return QString::number(n / (1024.0 * 1024), 'f', 1) + " MB";
    return QString::number(n / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
}

QString TaskCard::fmtSpeed(int bps) const {
    if (bps <= 0) return "";
    return fmtBytes(static_cast<quint64>(bps)) + "/s";
}

QString TaskCard::fmtDuration(quint64 ms) const {
    const auto secs = ms / 1000;
    if (secs < 60) return QStringLiteral("%1秒").arg(secs);
    if (secs < 3600) return QStringLiteral("%1分%2秒").arg(secs / 60).arg(secs % 60);
    return QStringLiteral("%1时%2分").arg(secs / 3600).arg((secs % 3600) / 60);
}

TaskCard::TaskCard(std::shared_ptr<AppState> state, TaskDto dto, QWidget* parent)
    : QWidget(parent), state_(std::move(state)), dto_(std::move(dto)) {
    setStyleSheet(
        "QWidget { background: #1c2330; border: 1px solid #ffffff20; border-radius: 10px; }");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* topRow = new QHBoxLayout();
    title_ = new QLabel();
    QFont tf; tf.setPointSize(11); tf.setBold(true);
    title_->setFont(tf);
    topRow->addWidget(title_);
    topRow->addStretch();
    status_ = new QLabel();
    status_->setStyleSheet("color: #7a8699; font-size: 11px");
    topRow->addWidget(status_);
    layout->addLayout(topRow);

    detail_ = new QLabel();
    detail_->setStyleSheet("color: #dfe5ef; font-size: 11px");
    detail_->setWordWrap(true);
    layout->addWidget(detail_);

    bar_ = new QProgressBar();
    bar_->setRange(0, 1000);
    bar_->setTextVisible(false);
    bar_->setMinimumHeight(8);
    bar_->setStyleSheet(
        "QProgressBar { background: #0f131b; border: 1px solid #ffffff20; border-radius: 4px; }"
        "QProgressBar::chunk { background: #4f8cff; border-radius: 3px; }");
    layout->addWidget(bar_);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();

    openBtn_ = new QPushButton("打开输出");
    previewBtn_ = new QPushButton("预览");
    pauseBtn_ = new QPushButton("暂停");
    cancelBtn_ = new QPushButton("取消");
    removeBtn_ = new QPushButton("删除");
    for (auto* b : {openBtn_, previewBtn_, pauseBtn_, cancelBtn_, removeBtn_}) {
        b->setStyleSheet(
            "QPushButton { padding: 4px 10px; border-radius: 4px; background: #222b3a;"
            "              color: #dfe5ef; border: 1px solid #ffffff20; }"
            "QPushButton:hover { background: #2c3645; }");
        btnRow->addWidget(b);
    }
    connect(cancelBtn_, &QPushButton::clicked, this, [this]() { emit cancelClicked(dto_.id); });
    connect(pauseBtn_,  &QPushButton::clicked, this, [this]() {
        if (dto_.status == "paused") emit resumeClicked(dto_.id);
        else emit pauseClicked(dto_.id);
    });
    connect(removeBtn_, &QPushButton::clicked, this, [this]() { emit removeClicked(dto_.id); });
    connect(openBtn_, &QPushButton::clicked, this, [this]() {
        emit openOutputClicked(dto_.output);
    });
    connect(previewBtn_, &QPushButton::clicked, this, [this]() {
        emit previewClicked(dto_.output);
    });
    layout->addLayout(btnRow);

    refresh();
}

void TaskCard::updateDto(const TaskDto& dto) {
    dto_ = dto;
    refresh();
}

void TaskCard::refresh() {
    const QFileInfo fi(dto_.source);
    title_->setText(QStringLiteral("#%1 · %2").arg(dto_.id).arg(fi.fileName()));
    auto colorFor = [](const QString& st) {
        if (st == "done") return QColor("#3fb950");
        if (st == "error") return QColor("#ff5555");
        if (st == "cancelled") return QColor("#b39268");
        if (st == "paused") return QColor("#d29922");
        if (st == "running") return QColor("#8fb4ff");
        return QColor("#7a8699");
    };
    status_->setText(dto_.status);
    status_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700")
                               .arg(colorFor(dto_.status).name()));

    const auto pct = (dto_.total_tiles > 0)
        ? static_cast<int>(1000.0 * dto_.tiles_done / dto_.total_tiles) : 0;
    bar_->setValue(pct);

    QStringList info;
    info << QStringLiteral("源: %1").arg(fi.absoluteFilePath());
    info << QStringLiteral("输出: %1").arg(dto_.output);
    info << QStringLiteral("瓦片: %1 / %2 (%3%)")
            .arg(dto_.tiles_done)
            .arg(dto_.total_tiles)
            .arg(dto_.total_tiles > 0 ? dto_.tiles_done * 100 / dto_.total_tiles : 0);
    const QString schemeStr = (dto_.scheme == Scheme::Tms) ? QStringLiteral("TMS") : QStringLiteral("XYZ");
    info << QStringLiteral("级别: %1 (Z%2)").arg(schemeStr).arg(dto_.level);
    if (dto_.bytes_written > 0) {
        info << QStringLiteral("已写: %1").arg(fmtBytes(dto_.bytes_written));
    }
    if (dto_.elapsed_ms > 0) {
        info << QStringLiteral("用时: %1").arg(fmtDuration(dto_.elapsed_ms));
    }
    const auto speed = state_ ? state_->speedBps(static_cast<int>(dto_.id)) : 0;
    if (speed > 0) info << QStringLiteral("速度: %1").arg(fmtSpeed(speed));
    if (dto_.error && !dto_.error->isEmpty()) info << QStringLiteral("错误: %1").arg(*dto_.error);

    detail_->setText(info.join("\n"));

    cancelBtn_->setEnabled(dto_.status == "running" || dto_.status == "queued" || dto_.status == "paused");
    pauseBtn_->setEnabled(dto_.status == "running" || dto_.status == "paused");
    pauseBtn_->setText(dto_.status == "paused" ? "恢复" : "暂停");
    removeBtn_->setEnabled(dto_.status != "running");
}

} // namespace qcutter
