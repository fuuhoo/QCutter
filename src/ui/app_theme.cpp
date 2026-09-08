#include "ui/app_theme.h"
#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

namespace qcutter {

void AppTheme::apply(bool dark) {
    qApp->setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    if (dark) {
        p.setColor(QPalette::Window,         QColor("#171c26"));
        p.setColor(QPalette::WindowText,     QColor("#dfe5ef"));
        p.setColor(QPalette::Base,           QColor("#0f131b"));
        p.setColor(QPalette::AlternateBase,  QColor("#1c2330"));
        p.setColor(QPalette::ToolTipBase,    QColor("#171c26"));
        p.setColor(QPalette::ToolTipText,    QColor("#dfe5ef"));
        p.setColor(QPalette::Text,           QColor("#dfe5ef"));
        p.setColor(QPalette::Button,         QColor("#222b3a"));
        p.setColor(QPalette::ButtonText,     QColor("#dfe5ef"));
        p.setColor(QPalette::BrightText,     QColor("#ffffff"));
        p.setColor(QPalette::Highlight,      QColor("#4f8cff"));
        p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        p.setColor(QPalette::Link,           QColor("#8fb4ff"));
        p.setColor(QPalette::PlaceholderText, QColor("#7a8699"));
    } else {
        p.setColor(QPalette::Window,         QColor("#f5f7fa"));
        p.setColor(QPalette::WindowText,     QColor("#1c2330"));
        p.setColor(QPalette::Base,           QColor("#ffffff"));
        p.setColor(QPalette::AlternateBase,  QColor("#eef1f6"));
        p.setColor(QPalette::ToolTipBase,    QColor("#171c26"));
        p.setColor(QPalette::ToolTipText,    QColor("#dfe5ef"));
        p.setColor(QPalette::Text,           QColor("#1c2330"));
        p.setColor(QPalette::Button,         QColor("#e2e8f0"));
        p.setColor(QPalette::ButtonText,     QColor("#1c2330"));
        p.setColor(QPalette::BrightText,     QColor("#000000"));
        p.setColor(QPalette::Highlight,      QColor("#4f8cff"));
        p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        p.setColor(QPalette::Link,           QColor("#1f6feb"));
        p.setColor(QPalette::PlaceholderText, QColor("#7a8699"));
    }
    qApp->setPalette(p);
}

QColor AppTheme::accentColor() { return QColor("#4f8cff"); }
QColor AppTheme::surfaceColor(bool dark) { return dark ? QColor("#171c26") : QColor("#f5f7fa"); }
QColor AppTheme::panelColor(bool dark) { return dark ? QColor("#1c2330") : QColor("#ffffff"); }

} // namespace qcutter