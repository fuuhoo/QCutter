// QCutter 引擎错误类型定义
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace qcutter {

/// 引擎核心错误。所有切片/读取失败的根因。
class CoreError : public std::runtime_error {
public:
    enum class Kind {
        TiffDecode,    ///< libtiff 解码失败
        Io,            ///< 文件 IO 错误
        Unsupported,   ///< 不支持的格式/参数
        InvalidInput,  ///< 参数无效
        Encoding,      ///< PNG 编码/像素转换错误
        Cancelled,     ///< 任务取消
        Db,            ///< SQLite 错误
        Http,          ///< HTTP 服务错误
    };

    CoreError(Kind kind, std::string msg)
        : std::runtime_error(std::move(msg)), kind_(kind) {}

    Kind kind() const noexcept { return kind_; }

    static CoreError io(std::string path, const std::string& source) {
        return CoreError(Kind::Io, "IO error (" + path + "): " + source);
    }
    static CoreError unsupported(std::string msg) {
        return CoreError(Kind::Unsupported, std::move(msg));
    }
    static CoreError invalid(std::string msg) {
        return CoreError(Kind::InvalidInput, std::move(msg));
    }
    static CoreError encoding(std::string msg) {
        return CoreError(Kind::Encoding, std::move(msg));
    }
    static CoreError cancelled() {
        return CoreError(Kind::Cancelled, "task cancelled");
    }
    static CoreError db(std::string msg) {
        return CoreError(Kind::Db, std::move(msg));
    }
    static CoreError tiff(const std::string& msg) {
        return CoreError(Kind::TiffDecode, "TIFF decode: " + msg);
    }

private:
    Kind kind_;
};

using CoreResult = std::variant<std::monostate, std::string, int>;
// 为简化错误传播, 采用异常方式. CoreResult 仅作为占位/未来扩展.

} // namespace qcutter