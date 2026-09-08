#include "util/paths.h"
#include <cstdlib>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>

namespace qcutter {

fs::path appDataDir() {
    // 优先使用 Qt 标准路径
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!base.isEmpty()) {
        return fs::path(base.toStdString());
    }
    return fs::path();
}

} // namespace qcutter