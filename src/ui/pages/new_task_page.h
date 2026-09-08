// 新建任务页: 左侧参数表单 + 右侧预览.
#pragma once
#include <QWidget>
#include <QString>
#include <QList>
#include <QByteArray>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QTableWidget>
#include <memory>

namespace qcutter {

class AppState;

struct TaskDraft {
    QString source;
    QString fileName;
    int width = 0;
    int height = 0;
    int maxLevel = 0;
    int tileSize = 256;
    QString scheme = "xyz";
    QString alphaJson = "{\"mode\":\"keep\"}";
    QString resample = "bilinear";
    int zmin = 0;
    int zmax = 0;
    QString outputDir;
    bool mercator = false;
    bool skipEmpty = false;
    bool precise = true;
    QByteArray previewPng;
    QString previewError;
    bool loadingPreview = false;
    bool pickColorMode = false;
    struct LevelEst {
        int level = 0;
        int width = 0;
        int height = 0;
        int tiles = 0;
        quint64 total = 0;
    };
    QList<LevelEst> estimates;
    QString estimateError;
};

class NewTaskPage : public QWidget {
    Q_OBJECT
public:
    explicit NewTaskPage(std::shared_ptr<AppState> state, QWidget* parent = nullptr);

private slots:
    void onPickSource();
    void onRunEstimate();
    void onStart();
    void onAlphaChanged(int idx);
    void onPreviewClicked(int x, int y);

private:
    void loadPreview(TaskDraft& draft);
    void refreshEstimates(TaskDraft& draft);
    void updateUiFromDraft();
    QWidget* buildFormPanel();
    QWidget* buildPreviewPanel();
    bool eventFilter(QObject* obj, QEvent* e) override;

    std::shared_ptr<AppState> state_;
    TaskDraft draft_;
    bool draftLoaded_ = false;

    QLineEdit* sourceEdit_ = nullptr;
    QSpinBox* zminSpin_ = nullptr;
    QSpinBox* zmaxSpin_ = nullptr;
    QSpinBox* tileSizeSpin_ = nullptr;
    QLineEdit* outputEdit_ = nullptr;
    QCheckBox* mercatorCheck_ = nullptr;
    QCheckBox* skipEmptyCheck_ = nullptr;
    QCheckBox* preciseCheck_ = nullptr;
    QComboBox* schemeCombo_ = nullptr;
    QComboBox* alphaCombo_ = nullptr;
    QLineEdit* ckColorEdit_ = nullptr;   // "r,g,b,tol"
    QPushButton* pickColorBtn_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QLabel* previewLabel_ = nullptr;
    QTableWidget* estimateTable_ = nullptr;
    QLabel* estimateErrorLabel_ = nullptr;  // 估算错误时持久显示在 UI 上 (不再仅弹 dialog)
    QLabel* resampleLabel_ = nullptr;       // 当前全局重采样 (设置页改, 这里只读)
    QCursor pickCursor_;                    // 取色模式下预览图上的十字吸管光标

    static QImage makePickerCursorImage();  // 19x19 黑白描边 + 中心十字
    static QImage makeCheckerImage(int w, int h);  // 浅灰底 + 中灰棋盘格 (16px)
};

} // namespace qcutter