// 应用数据目录解析 (Windows / macOS / Linux).
#pragma once
#include "util/fs_compat.h"

namespace qcutter {

inline constexpr const char* APP_DIR_NAME = "QCutter";

/// 返回应用专属数据目录的路径 (不保证已存在).
fs::path appDataDir();

} // namespace qcutter