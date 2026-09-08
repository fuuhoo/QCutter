// 静态 HTTP 预览服务: 为切片输出目录提供 127.0.0.1:<port> 访问.
#pragma once
#include <cstdint>
#include <experimental/filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class QTcpServer;

namespace qcutter {

class PreviewServer {
public:
    /// 为目录启动 (或复用) 静态服务, 返回端口号. 失败抛异常.
    static std::uint16_t serve(const std::experimental::filesystem::path& dir);

private:
    PreviewServer() = default;
    static PreviewServer& instance();

    std::mutex mu_;
    std::map<std::experimental::filesystem::path,
             std::pair<std::uint16_t, std::shared_ptr<QTcpServer>>> entries_;
};

} // namespace qcutter