// QCutter 应用主题: 与 swCutter Flutter 端一致的暗/亮配色.
#pragma once
#include <QString>

class QColor;

namespace qcutter {

/// 应用主题单例.
class AppTheme {
public:
    /// 加载并应用主题到 QApplication.
    static void apply(bool dark = true);

    /// 主题颜色 (供 widgets 使用).
    static QColor accentColor();
    static QColor surfaceColor(bool dark);
    static QColor panelColor(bool dark);
};

} // namespace qcutter