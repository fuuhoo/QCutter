#include "ui/pages/settings_page.h"
#include "state/app_state.h"
#include "engine/planner.h"
#include "ui/app_theme.h"

#include <QFileDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QStandardPaths>

namespace qcutter {

SettingsPage::SettingsPage(std::shared_ptr<AppState> state, QWidget* parent)
    : QWidget(parent), state_(std::move(state)) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(10);

    auto* header = new QLabel("设置");
    QFont hf; hf.setPointSize(13); hf.setBold(true);
    header->setFont(hf);
    root->addWidget(header);

    // ---- 全局参数 ----
    auto* gp = new QGroupBox("全局参数");
    auto* gf = new QFormLayout(gp);

    auto* concurrencySpin = new QSpinBox();
    concurrencySpin->setRange(1, 16);
    concurrencySpin->setValue(state_->concurrency());
    connect(concurrencySpin, qOverload<int>(&QSpinBox::valueChanged), state_.get(), &AppState::setConcurrency);
    gf->addRow("最大并发任务", concurrencySpin);

    auto* defaultOutputEdit = new QLineEdit(state_->defaultOutput());
    connect(defaultOutputEdit, &QLineEdit::editingFinished, this, [this, defaultOutputEdit]() {
        state_->setDefaultOutput(defaultOutputEdit->text());
    });
    auto* outputRow = new QHBoxLayout();
    outputRow->addWidget(defaultOutputEdit);
    auto* pickBtn = new QPushButton("浏览…");
    outputRow->addWidget(pickBtn);
    connect(pickBtn, &QPushButton::clicked, this, [this, defaultOutputEdit]() {
        const QString p = QFileDialog::getExistingDirectory(this, "选择默认输出目录",
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
        if (!p.isEmpty()) {
            defaultOutputEdit->setText(p);
            state_->setDefaultOutput(p);
        }
    });
    gf->addRow("默认输出目录", outputRow);

    auto* tileSizeSpin = new QSpinBox();
    tileSizeSpin->setRange(64, 1024);
    tileSizeSpin->setSingleStep(128);
    tileSizeSpin->setValue(state_->tileSize());
    connect(tileSizeSpin, qOverload<int>(&QSpinBox::valueChanged), state_.get(), &AppState::setTileSize);
    gf->addRow("默认瓦片尺寸", tileSizeSpin);

    auto* skipEmptyCheck = new QCheckBox("默认跳过全透明瓦片");
    skipEmptyCheck->setChecked(state_->skipEmpty());
    connect(skipEmptyCheck, &QCheckBox::toggled, state_.get(), &AppState::setSkipEmpty);
    gf->addRow(skipEmptyCheck);

    auto* resampleCombo = new QComboBox();
    resampleCombo->addItems({"nearest", "bilinear"});
    resampleCombo->setCurrentText(state_->resample() == Resample::Nearest ? "nearest" : "bilinear");
    connect(resampleCombo, &QComboBox::currentTextChanged, state_.get(), [this](const QString& v) {
        state_->setResample(v == "nearest" ? Resample::Nearest : Resample::Bilinear);
    });
    gf->addRow("默认重采样", resampleCombo);

    auto* darkCheck = new QCheckBox("使用暗色主题");
    darkCheck->setChecked(state_->darkTheme());
    connect(darkCheck, &QCheckBox::toggled, state_.get(), [this](bool v) {
        state_->setDarkTheme(v);
        // 主题切换需重启应用生效
    });
    gf->addRow(darkCheck);

    root->addWidget(gp);

    // ---- 地图密钥 ----
    auto* kpg = new QGroupBox("地图服务密钥 (用于底图模板占位符)");
    auto* kfl = new QFormLayout(kpg);
    const auto keys = state_->mapKeys();
    for (const auto& kv : keys) {
        const auto eq = kv.indexOf('=');
        if (eq <= 0) continue;
        const QString name = kv.left(eq);
        const QString value = kv.mid(eq + 1);
        auto* edit = new QLineEdit(value);
        kfl->addRow(name + ":", edit);
        connect(edit, &QLineEdit::editingFinished, this, [this, name, edit]() {
            state_->setMapKey(name, edit->text());
        });
    }
    root->addWidget(kpg);

    // ---- 底图列表 ----
    // 底图改为内置固定列表 (OSM / 天地图), 用户只能勾选 / 切底图/叠加 / 切 TMS,
    // 不能新增/删除/编辑 URL. 列拖动通过 InteractiveSectionResizeMode 启用.
    auto* bpg = new QGroupBox("底图 / 叠加层 (用于预览)");
    auto* bpl = new QVBoxLayout(bpg);

    auto* tbl = new QTableWidget();
    tbl->setColumnCount(5);
    tbl->setHorizontalHeaderLabels({"名称", "URL 模板", "TMS", "显示", "底图/叠加"});
    // Interactive: 列宽可拖动. URL 模板这一列宽一些以容纳完整 URL.
    tbl->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    tbl->horizontalHeader()->setStretchLastSection(false);
    tbl->setColumnWidth(0, 140);   // 名称
    tbl->setColumnWidth(1, 360);   // URL 模板
    tbl->setColumnWidth(2, 50);    // TMS
    tbl->setColumnWidth(3, 50);    // 显示
    tbl->setColumnWidth(4, 90);    // 底图/叠加
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 只读: 不允许直接编辑 URL 模板. URL 是预定义的, 不可改.
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // 把 tk= 这种密钥自动填到 URL 模板的 {tk} 占位符. 先在组里抓 tk 密钥.
    auto tkFor = [this]() -> QString {
        for (const auto& kv : state_->mapKeys()) {
            const auto eq = kv.indexOf('=');
            if (eq <= 0) continue;
            const auto name = kv.left(eq);
            if (name == "tk") return kv.mid(eq + 1);
        }
        return QString();
    };

    auto refreshTbl = [tbl, this, tkFor]() {
        const auto bms = state_->baseMapks();
        tbl->setRowCount(bms.size());
        const auto tk = tkFor();
        for (int i = 0; i < bms.size(); ++i) {
            const auto m = bms[i].toMap();
            const auto tpl_raw = m.value("tpl").toString();
            // 把密钥代入 {tk} 占位符, 让用户看到替换后的真实 URL.
            QString tpl = tpl_raw;
            tpl.replace(QStringLiteral("{tk}"), tk);

            auto* name_item = new QTableWidgetItem(m.value("name").toString());
            name_item->setFlags(name_item->flags() & ~Qt::ItemIsEditable);
            tbl->setItem(i, 0, name_item);

            auto* tpl_item = new QTableWidgetItem(tpl);
            tpl_item->setFlags(tpl_item->flags() & ~Qt::ItemIsEditable);
            tpl_item->setToolTip(tpl); // 鼠标悬停显示完整 URL
            QFont mf; mf.setStyleHint(QFont::Monospace);
            tpl_item->setFont(mf);
            tbl->setItem(i, 1, tpl_item);

            const auto key = m.value("key").toString();
            auto* tms = new QCheckBox();
            tms->setChecked(m.value("tms").toBool());
            // 防 lambda 闭包悬挂: 捕获 key 而非 i (key 稳定)
            connect(tms, &QCheckBox::toggled, this, [this, key](bool v) {
                state_->setBasemapTms(key, v);
            });
            tbl->setCellWidget(i, 2, tms);

            auto* on = new QCheckBox();
            on->setChecked(m.value("on").toBool());
            connect(on, &QCheckBox::toggled, this, [this, key](bool v) {
                state_->setBasemapOn(key, v);
            });
            tbl->setCellWidget(i, 3, on);

            auto* below = new QCheckBox();
            below->setChecked(m.value("below").toBool());
            connect(below, &QCheckBox::toggled, this, [this, key](bool v) {
                state_->setBasemapBelow(key, v);
            });
            tbl->setCellWidget(i, 4, below);
        }
    };
    refreshTbl();

    bpl->addWidget(tbl);

    // 底图说明: 解释 OSM 和天地图的差异, 提示 URL 不可编辑.
    auto* hint = new QLabel("提示: 内置底图 (OpenStreetMap / 天地图), URL 模板只读不可改. "
                            "拖动列分隔线可调整列宽; 修改密钥后表格会自动更新 URL.");
    hint->setStyleSheet("color: #7a8699; font-size: 11px");
    hint->setWordWrap(true);
    bpl->addWidget(hint);

    // 改 tk 密钥后刷新表格里 {tk} 占位符的展开. 监听 state 改变.
    connect(state_.get(), &AppState::changed, this, [refreshTbl, tbl, this]() {
        // 简单做法: 整表重建, 但要保留行选中.
        const auto sel = tbl->currentRow();
        refreshTbl();
        if (sel >= 0 && sel < tbl->rowCount()) tbl->selectRow(sel);
    });

    root->addWidget(bpg);

    root->addStretch();
}

} // namespace qcutter