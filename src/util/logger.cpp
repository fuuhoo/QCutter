#include "util/logger.h"
#include "util/paths.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <iostream>

namespace qcutter {

void logWrite(const QString& level, const QString& msg) {
    const auto dir = appDataDir() / "logs";
    QDir().mkpath(QString::fromStdString(dir.string()));
    const auto file = dir / "qcutter.log";
    QFile f(QString::fromStdString(file.string()));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << "[" << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz") << "]"
           << " [" << level << "] " << msg << "\n";
    } else {
        std::cerr << "[" << level.toStdString() << "] " << msg.toStdString() << std::endl;
    }
}

} // namespace qcutter