// 轻量日志: 追加到 <appDataDir>/logs/qcutter.log.
#pragma once
#include <QString>

namespace qcutter {

/// 写一行日志. level = "info" / "error" / "warn" / "debug".
/// 内部使用 std::cerr 兜底输出, 文件写失败时静默忽略.
void logWrite(const QString& level, const QString& msg);

} // namespace qcutter

#define LOG_I(tag, msg) ::qcutter::logWrite("info", QStringLiteral("[" tag "] ") + msg)
#define LOG_E(tag, msg) ::qcutter::logWrite("error", QStringLiteral("[" tag "] ") + msg)
#define LOG_W(tag, msg) ::qcutter::logWrite("warn", QStringLiteral("[" tag "] ") + msg)