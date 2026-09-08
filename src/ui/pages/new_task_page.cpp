#include "ui/pages/new_task_page.h"
#include "state/app_state.h"
#include "api/task_manager.h"
#include "engine/alpha.h"
#include "engine/planner.h"
#include "engine/source.h"
#include "util/logger.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QGroupBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QImage>
#include <QPixmap>
#include <QCursor>
#include <QMouseEvent>
#include <QMessageBox>
#include <QStandardPaths>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpressionValidator>
#include <QValidator>

#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>

#include <experimental/filesystem>

namespace qcutter {

namespace fs = std::experimental::filesystem;

// --- TaskDraft helpers ---

static QString fileBaseName(const QString& path) {
    QFileInfo fi(path);
    return fi.completeBaseName();
}

static QString defaultOutputFor(const QString& sourcePath,
                                 const QString& globalDefault) {
    QFileInfo fi(sourcePath);
    const QString sep = "/";
    const QString stem = fi.completeBaseName();
    const QString base = globalDefault.isEmpty() ? fi.absolutePath() : globalDefault;
    const QString tail = base.endsWith('/') ? base : (base + sep);
    return tail + stem;
}

// --- NewTaskPage ---

QImage NewTaskPage::makePickerCursorImage() {
    // 经典颜色拾取器光标: 25x25 黑色描边白色填充吸管, 中心精确 1px.
    // hot 设在中心 (12, 12). 不依赖系统主题, 跨平台一致.
    QImage img(25, 25, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    // 外圆 (吸管轮廓)
    p.drawEllipse(QPoint(12, 12), 8, 8);
    // 中心精确 1px 十字 (对齐像素)
    p.setPen(QPen(Qt::red, 1));
    p.drawLine(12, 4,  12, 9);
    p.drawLine(12, 15, 12, 20);
    p.drawLine(4,  12, 9,  12);
    p.drawLine(15, 12, 20, 12);
    p.end();
    return img;
}

QImage NewTaskPage::makeCheckerImage(int w, int h) {
    // 透明 PNG 在 UI 上需要"底图"显示缺图/透明区.
    // 经典棋盘格: 浅灰底 + 中灰格. 16px 大格子. 任何深色图像都不会和它混淆.
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(0xc8, 0xc8, 0xc8));
    QPainter p(&img);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xa8, 0xa8, 0xa8));
    for (int y = 0; y < h; y += 16) {
        for (int x = 0; x < w; x += 16) {
            p.drawRect(x,     y,     8, 8);
            p.drawRect(x + 8, y + 8, 8, 8);
        }
    }
    p.end();
    return img;
}

NewTaskPage::NewTaskPage(std::shared_ptr<AppState> state, QWidget* parent)
    : QWidget(parent), state_(std::move(state)) {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->addWidget(buildFormPanel(), 0);
    root->addWidget(buildPreviewPanel(), 1);
    connect(state_.get(), &AppState::changed, this, [this]() { updateUiFromDraft(); });
}

QWidget* NewTaskPage::buildFormPanel() {
    auto* panel = new QWidget();
    panel->setFixedWidth(420);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* header = new QLabel("新建切片任务");
    QFont hf; hf.setPointSize(13); hf.setBold(true);
    header->setFont(hf);
    layout->addWidget(header);

    auto* pickBtn = new QPushButton("选择 GeoTIFF…");
    layout->addWidget(pickBtn);
    connect(pickBtn, &QPushButton::clicked, this, &NewTaskPage::onPickSource);

    sourceEdit_ = new QLineEdit();
    sourceEdit_->setReadOnly(true);
    sourceEdit_->setPlaceholderText("尚未选择文件");
    layout->addWidget(sourceEdit_);

    infoLabel_ = new QLabel(" ");
    infoLabel_->setStyleSheet("color: #7a8699; font-size: 11px");
    infoLabel_->setWordWrap(true);
    layout->addWidget(infoLabel_);

    auto* basic = new QGroupBox("基础参数");
    auto* form = new QFormLayout(basic);
    form->setLabelAlignment(Qt::AlignRight);

    tileSizeSpin_ = new QSpinBox();
    tileSizeSpin_->setRange(64, 1024);
    tileSizeSpin_->setSingleStep(128);
    tileSizeSpin_->setValue(state_->tileSize());
    connect(tileSizeSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        draft_.tileSize = v;
        // tile_size 改变会影响切片数估算
        if (draftLoaded_) onRunEstimate();
    });
    form->addRow("瓦片尺寸", tileSizeSpin_);

    zminSpin_ = new QSpinBox();
    zminSpin_->setRange(0, 22);
    zminSpin_->setValue(0);
    connect(zminSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        draft_.zmin = v;
        if (draft_.zmax < v) { draft_.zmax = v; zmaxSpin_->setValue(v); }
        // 立即重算估算表 (zmin/zmax 改了)
        if (draftLoaded_) onRunEstimate();
    });
    form->addRow("Z 最小", zminSpin_);

    zmaxSpin_ = new QSpinBox();
    zmaxSpin_->setRange(0, 22);
    zmaxSpin_->setValue(0);
    connect(zmaxSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        draft_.zmax = v;
        // 立即重算估算表
        if (draftLoaded_) onRunEstimate();
    });
    form->addRow("Z 最大", zmaxSpin_);

    schemeCombo_ = new QComboBox();
    schemeCombo_->addItems({"xyz", "tms"});
    form->addRow("瓦片方案", schemeCombo_);

    // 重采样已挪到全局设置 (settings_page): 默认重采样.
    // 这里只显示当前全局值, 改了设置后自动同步.
    resampleLabel_ = new QLabel();
    resampleLabel_->setStyleSheet("color:#dfe5ef;font-weight:600");
    resampleLabel_->setText(QString::fromStdString(resampleToString(state_->resample())));
    {
        auto* row = new QHBoxLayout();
        row->addWidget(resampleLabel_);
        row->addWidget(new QLabel("  (在「设置」页修改)"));
        row->addStretch();
        auto* container = new QWidget();
        container->setLayout(row);
        form->addRow("重采样", container);
    }

    layout->addWidget(basic);

    auto* opt = new QGroupBox("高级");
    auto* oform = new QFormLayout(opt);

    mercatorCheck_ = new QCheckBox("Mercator 绝对级别模式 (GDAL 风格)");
    mercatorCheck_->setChecked(true);
    connect(mercatorCheck_, &QCheckBox::toggled, this, [this](bool v) {
        draft_.mercator = v;
        // 切换 mercator 时, georef 是否可用决定估算路径, 必须重算
        if (draftLoaded_) onRunEstimate();
    });
    oform->addRow(mercatorCheck_);

    skipEmptyCheck_ = new QCheckBox("跳过全透明瓦片");
    skipEmptyCheck_->setChecked(false);
    connect(skipEmptyCheck_, &QCheckBox::toggled, [&](bool v) {
        draft_.skipEmpty = v;
    });
    oform->addRow(skipEmptyCheck_);

    preciseCheck_ = new QCheckBox("精确反算 (proj4rs 实时, 推荐开启)");
    preciseCheck_->setChecked(true);
    oform->addRow(preciseCheck_);

    alphaCombo_ = new QComboBox();
    alphaCombo_->addItems({"keep 保留", "threshold 阈值", "colorkey 颜色键"});
    connect(alphaCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &NewTaskPage::onAlphaChanged);
    oform->addRow("透明模式", alphaCombo_);

    ckColorEdit_ = new QLineEdit("0,0,0,12");
    auto* rx = new QRegularExpressionValidator(QRegularExpression("\\d{1,3},\\d{1,3},\\d{1,3},\\d{1,3}"), ckColorEdit_);
    ckColorEdit_->setValidator(rx);
    ckColorEdit_->setEnabled(false);
    oform->addRow("颜色键 (r,g,b,tol)", ckColorEdit_);

    pickColorBtn_ = new QPushButton("从预览取色…");
    pickColorBtn_->setEnabled(false);
    pickColorBtn_->setCheckable(true);
    // 经典颜色拾取器光标: 18x18 黑色描边白色填充吸管, 中心精确 1px.
    // 用 SVG data URI 内嵌到 QCursor, 跨平台一致.
    pickCursor_ = QCursor(QPixmap::fromImage(makePickerCursorImage()));
    connect(pickColorBtn_, &QPushButton::toggled, [&](bool v) {
        draft_.pickColorMode = v;
        if (previewLabel_) {
            previewLabel_->setCursor(v ? pickCursor_ : Qt::ArrowCursor);
        }
    });
    oform->addRow("", pickColorBtn_);

    layout->addWidget(opt);

    auto* out = new QGroupBox("输出");
    auto* oform2 = new QFormLayout(out);
    auto* outputRow = new QHBoxLayout();
    outputEdit_ = new QLineEdit();
    // 不再 setReadOnly, 允许直接编辑路径. 浏览按钮可手动选择.
    outputRow->addWidget(outputEdit_, 1);
    auto* pickOutBtn = new QPushButton("浏览…");
    outputRow->addWidget(pickOutBtn);
    // 用户编辑或选择目录后, 同步回 draft_.outputDir, 这样「开始切割」用的就是新路径.
    connect(outputEdit_, &QLineEdit::editingFinished, this, [this]() {
        draft_.outputDir = outputEdit_->text();
    });
    connect(pickOutBtn, &QPushButton::clicked, this, [this]() {
        const QString base = outputEdit_->text().isEmpty()
            ? state_->defaultOutput() : outputEdit_->text();
        const QString start = QDir(base).exists() ? base
            : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        const QString p = QFileDialog::getExistingDirectory(this, "选择输出目录", start);
        if (!p.isEmpty()) {
            outputEdit_->setText(p);
            draft_.outputDir = p;
        }
    });
    oform2->addRow("输出目录", outputRow);
    layout->addWidget(out);

    estimateTable_ = new QTableWidget();
    estimateTable_->setColumnCount(4);
    estimateTable_->setHorizontalHeaderLabels({"级别", "宽×高", "瓦片数", "累计"});
    estimateTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    estimateTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    estimateTable_->setSelectionMode(QAbstractItemView::NoSelection);
    estimateTable_->setFocusPolicy(Qt::NoFocus);
    estimateTable_->verticalHeader()->setVisible(false);
    estimateTable_->setMinimumHeight(180);
    layout->addWidget(estimateTable_);

    // 估算错误提示: 持久显示在 estimateTable 下方, 红色文字 + 警告图标.
    // 比单次 dialog 友好, 用户能看到上下文 (已选文件/参数) 再修正.
    estimateErrorLabel_ = new QLabel();
    estimateErrorLabel_->setVisible(false);
    estimateErrorLabel_->setWordWrap(true);
    estimateErrorLabel_->setTextFormat(Qt::RichText);
    estimateErrorLabel_->setStyleSheet(
        "color:#ffb4b4;background:#3a1010e6;border:1px solid #ff555544;"
        "border-radius:6px;padding:8px 10px;font-size:12px");
    layout->addWidget(estimateErrorLabel_);

    startBtn_ = new QPushButton("开始切片");
    startBtn_->setEnabled(false);
    connect(startBtn_, &QPushButton::clicked, this, &NewTaskPage::onStart);
    layout->addWidget(startBtn_);

    layout->addStretch();
    return panel;
}

QWidget* NewTaskPage::buildPreviewPanel() {
    auto* panel = new QWidget();
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* header = new QLabel("预览 (alpha 应用后)");
    QFont hf; hf.setPointSize(11);
    header->setFont(hf);
    layout->addWidget(header);

    previewLabel_ = new QLabel("尚未选择文件");
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setMinimumSize(360, 360);
    // 棋盘格: 经典 GIMP/Photoshop 透明表示.
    // 浅灰底 #c8c8c8 + 中灰格 #a8a8a8, 16px 大格子, 任何深色图像都不会和它混淆.
    previewLabel_->setStyleSheet(
        "background-color: #c8c8c8;"
        "background-image:"
        "  linear-gradient(45deg,#a8a8a8 25%,transparent 25%),"
        "  linear-gradient(-45deg,#a8a8a8 25%,transparent 25%),"
        "  linear-gradient(45deg,transparent 75%,#a8a8a8 75%),"
        "  linear-gradient(-45deg,transparent 75%,#a8a8a8 75%);"
        "background-size: 16px 16px;"
        "background-position: 0 0, 0 8px, 8px -8px, -8px 0;"
        "border: 1px solid #ffffff20; border-radius: 8px;"
        "color: #555;");
    previewLabel_->installEventFilter(this);
    layout->addWidget(previewLabel_, 1);

    return panel;
}

void NewTaskPage::onPickSource() {
    const QString path = QFileDialog::getOpenFileName(
        this, "选择 GeoTIFF 文件",
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        "TIFF 文件 (*.tif *.tiff)");
    if (path.isEmpty()) return;

    sourceEdit_->setText(path);
    draft_ = {};
    draft_.source = path;
    draft_.fileName = QFileInfo(path).fileName();
    draft_.tileSize = state_->tileSize();
    draft_.skipEmpty = state_->skipEmpty();
    draft_.resample = state_->resample() == Resample::Nearest ? "nearest" : "bilinear";
    draft_.mercator = true;
    draft_.zmin = 1;
    draft_.alphaJson = "{\"mode\":\"keep\"}";
    draft_.precise = true;

    try {
        const auto brief = TaskManager::readImageInfo(path);
        draft_.width = static_cast<int>(brief.width);
        draft_.height = static_cast<int>(brief.height);
        draft_.maxLevel = static_cast<int>(brief.max_level);
        infoLabel_->setText(QStringLiteral("%1×%2, %3 像素/%4样本, %5 压缩, %6 分块 (chunks %7×%8)")
                                .arg(brief.width).arg(brief.height)
                                .arg(brief.pixel_format)
                                .arg(brief.bits_per_sample == 0 ? "?" : QString::number(brief.bits_per_sample))
                                .arg(brief.compression)
                                .arg(brief.chunk_type)
                                .arg(brief.chunk_w).arg(brief.chunk_h));
        draft_.zmax = draft_.maxLevel;

        // 输出目录
        const QString sep = "/";
        const QString stem = QFileInfo(path).completeBaseName();
        const QString base = state_->defaultOutput().isEmpty()
                                 ? QFileInfo(path).absolutePath()
                                 : state_->defaultOutput();
        const QString baseN = base.endsWith(sep) ? base : (base + sep);
        draft_.outputDir = baseN + stem;
        outputEdit_->setText(draft_.outputDir);

        zminSpin_->setValue(draft_.zmin);
        zmaxSpin_->setValue(draft_.zmax);
        tileSizeSpin_->setValue(draft_.tileSize);
        mercatorCheck_->setChecked(draft_.mercator);
        skipEmptyCheck_->setChecked(draft_.skipEmpty);
        preciseCheck_->setChecked(draft_.precise);
        startBtn_->setEnabled(true);

        draftLoaded_ = true;
        loadPreview(draft_);
        onRunEstimate();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "读取失败", QString::fromUtf8(e.what()));
    }
}

void NewTaskPage::loadPreview(TaskDraft& draft) {
    draft.loadingPreview = true;
    previewLabel_->setText("加载预览中…");
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    // 关键: 通过 this 访问 widget 成员 draft_, 而不是形参 draft 的地址
    // (loadPreview 返回后形参 draft 析构, 闭包里的 draftPtr 会变成野指针导致 use-after-free)
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher]() {
        try {
            const QByteArray ba = watcher->result();
            draft_.previewPng = ba;
            QImage img;
            img.loadFromData(ba);
            // 把 PNG (含透明区) 画到一张预先填充棋盘格的底图上,
            // 这样透明像素透出棋盘格而非黑/白. PNG 中黑色图像边与棋盘格可清楚分辨.
            QImage checker = makeCheckerImage(
                previewLabel_->width() - 8, previewLabel_->height() - 8);
            QImage scaled = img.scaled(checker.width(), checker.height(),
                                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPainter p(&checker);
            // 居中绘制: checker 比 scaled 大时, 实际图像居中, 周围是棋盘格
            const int dx = (checker.width()  - scaled.width())  / 2;
            const int dy = (checker.height() - scaled.height()) / 2;
            p.drawImage(dx, dy, scaled);
            p.end();
            previewLabel_->setPixmap(QPixmap::fromImage(checker));
        } catch (...) {}
        watcher->deleteLater();
    });
    auto fut = QtConcurrent::run([draft]() {
        return TaskManager::makePreview(draft.source, 720,
                                        draft.alphaJson == "{\"mode\":\"keep\"}"
                                            ? AlphaMode::keep()
                                            : AlphaMode::fromJson(nlohmann::json::parse(draft.alphaJson.toStdString())));
    });
    watcher->setFuture(fut);
}

void NewTaskPage::onRunEstimate() {
    if (!draftLoaded_) return;
    AlphaMode a;
    try { a = AlphaMode::fromJson(nlohmann::json::parse(draft_.alphaJson.toStdString())); }
    catch (...) { a = AlphaMode::keep(); }
    Q_UNUSED(a);
    try {
        auto pe = TaskManager::estimatePyramidEx(
            draft_.source,
            static_cast<std::uint32_t>(draft_.width),
            static_cast<std::uint32_t>(draft_.height),
            static_cast<std::uint32_t>(draft_.tileSize),
            static_cast<std::uint32_t>(draft_.zmin),
            static_cast<std::uint32_t>(draft_.zmax),
            draft_.mercator);
        estimateTable_->setRowCount(static_cast<int>(pe.levels.size()));
        quint64 cum = 0;
        for (int i = 0; i < pe.levels.size(); ++i) {
            const auto& lv = pe.levels[i];
            cum += lv.tiles;
            estimateTable_->setItem(i, 0, new QTableWidgetItem(QString::number(lv.level)));
            estimateTable_->setItem(i, 1, new QTableWidgetItem(
                QStringLiteral("%1 × %2").arg(lv.width).arg(lv.height)));
            estimateTable_->setItem(i, 2, new QTableWidgetItem(QString::number(lv.tiles)));
            estimateTable_->setItem(i, 3, new QTableWidgetItem(QString::number(cum)));
        }
        // 估算成功: 隐藏错误标签, 启用开始按钮.
        estimateErrorLabel_->setVisible(false);
        estimateErrorLabel_->clear();
        draft_.estimateError.clear();
        startBtn_->setEnabled(true);
    } catch (const std::exception& e) {
        // 估算失败: 1) 清空估算表; 2) 在 UI 上显示错误; 3) 禁用开始按钮.
        // 保留弹窗, 但 UI 上有持久提示 (修正参数时方便).
        estimateTable_->setRowCount(0);
        const QString msg = QString::fromUtf8(e.what());
        const QString advice = draft_.mercator
            ? QStringLiteral("提示: 若该 TIFF 无 GeoTIFF 地理参考, 请取消勾选 \"Web Mercator\" 再试.")
            : QString();
        estimateErrorLabel_->setText(QStringLiteral(
            "⚠ 估算失败: %1%2").arg(msg.toHtmlEscaped()).arg(advice.isEmpty() ? QString() : "<br>" + advice.toHtmlEscaped()));
        estimateErrorLabel_->setVisible(true);
        draft_.estimateError = msg;
        startBtn_->setEnabled(false);
        QMessageBox::information(this, "估算失败", msg + (advice.isEmpty() ? QString() : ("\n\n" + advice)));
    }
}

void NewTaskPage::onAlphaChanged(int idx) {
    ckColorEdit_->setEnabled(idx == 2);
    pickColorBtn_->setEnabled(idx == 2);
    switch (idx) {
    case 0: draft_.alphaJson = "{\"mode\":\"keep\"}"; break;
    case 1: {
        const auto parts = ckColorEdit_->text().split(',');
        const int threshold = parts.size() >= 4 ? parts[3].toInt() : 128;
        draft_.alphaJson = QStringLiteral("{\"mode\":\"threshold\",\"value\":{\"below\":%1}}")
                               .arg(parts.size() >= 4 ? parts[3].toInt() : 128);
        Q_UNUSED(threshold);
        break;
    }
    case 2: {
        const auto parts = ckColorEdit_->text().split(',');
        const int r = parts.value(0).toInt();
        const int g = parts.value(1).toInt();
        const int b = parts.value(2).toInt();
        const int tol = parts.value(3).toInt();
        draft_.alphaJson = QStringLiteral(R"({"mode":"colorkey","value":{"r":%1,"g":%2,"b":%3,"tolerance":%4}})")
                               .arg(r).arg(g).arg(b).arg(tol);
        break;
    }
    }
    if (draftLoaded_) loadPreview(draft_);
}

void NewTaskPage::onPreviewClicked(int x, int y) {
    if (!draft_.pickColorMode || alphaCombo_->currentIndex() != 2) return;
    if (previewLabel_->pixmap() == nullptr || previewLabel_->pixmap()->isNull()) return;
    const auto* pm = previewLabel_->pixmap();
    const auto pix = pm->copy();
    const auto origImg = pix.toImage();
    const auto off = previewLabel_->mapFromGlobal(QPoint(x, y));
    if (off.x() < 0 || off.y() < 0) return;
    const auto imgW = origImg.width();
    const auto imgH = origImg.height();
    const auto pixW = pm->width();
    const auto pixH = pm->height();
    const auto offX = (previewLabel_->width() - pixW) / 2;
    const auto offY = (previewLabel_->height() - pixH) / 2;
    const auto lx = off.x() - offX;
    const auto ly = off.y() - offY;
    if (lx < 0 || ly < 0 || lx >= pixW || ly >= pixH) return;
    const auto srcX = lx * imgW / pixW;
    const auto srcY = ly * imgH / pixH;
    const auto color = origImg.pixel(srcX, srcY);
    const auto r = qRed(color), g = qGreen(color), b = qBlue(color);
    ckColorEdit_->setText(QStringLiteral("%1,%2,%3,12").arg(r).arg(g).arg(b));
    draft_.pickColorMode = false;
    pickColorBtn_->setChecked(false);
    onAlphaChanged(alphaCombo_->currentIndex());
}

bool NewTaskPage::eventFilter(QObject* obj, QEvent* e) {
    if (obj == previewLabel_ && e->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        onPreviewClicked(me->globalPos().x(), me->globalPos().y());
        return true;
    }
    return QWidget::eventFilter(obj, e);
}

void NewTaskPage::updateUiFromDraft() {
    // 同步设置页改的全局默认值到当前 UI 标签.
    if (resampleLabel_) {
        resampleLabel_->setText(QString::fromStdString(resampleToString(state_->resample())));
    }
}

void NewTaskPage::onStart() {
    if (draft_.source.isEmpty()) {
        QMessageBox::warning(this, "未选择文件", "请先选择 GeoTIFF 文件。");
        return;
    }
    TaskConfig cfg;
    cfg.source = draft_.source;
    cfg.output = draft_.outputDir;
    cfg.tile_size = static_cast<std::uint32_t>(draft_.tileSize);
    cfg.zmin = static_cast<std::uint32_t>(draft_.zmin);
    cfg.zmax = static_cast<std::uint32_t>(draft_.zmax);
    cfg.scheme = (schemeCombo_->currentText() == "tms") ? Scheme::Tms : Scheme::Xyz;
    try { cfg.alpha = AlphaMode::fromJson(nlohmann::json::parse(draft_.alphaJson.toStdString())); }
    catch (...) { cfg.alpha = AlphaMode::keep(); }
    // 重采样统一取 AppState 全局值 (见 settings_page)
    cfg.resample = state_->resample();
    cfg.skip_empty = skipEmptyCheck_->isChecked();
    cfg.mercator = mercatorCheck_->isChecked();
    cfg.precise = preciseCheck_->isChecked();
    cfg.preview_overlays = state_->overlaysJson();

    try {
        auto id = TaskManager::instance().startTask(cfg);
        QMessageBox::information(this, "已入队",
            QStringLiteral("任务 #%1 已加入队列。\n请在「任务中心」查看进度。").arg(id));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "启动失败", QString::fromUtf8(e.what()));
    }
}

} // namespace qcutter