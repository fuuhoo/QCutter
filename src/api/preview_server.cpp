#include "api/preview_server.h"
#include "engine/error.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QString>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QHostAddress>
#include <QDir>
#include <memory>

namespace qcutter {

PreviewServer& PreviewServer::instance() {
    static PreviewServer s;
    return s;
}

namespace {
class Handler : public QObject {
public:
    Handler(QTcpSocket* sock, const QString& root, QObject* parent = nullptr)
        : QObject(parent), sock_(sock), root_(root) {
        connect(sock_, &QTcpSocket::readyRead, this, &Handler::onReadyRead);
        connect(sock_, &QTcpSocket::disconnected, this, &Handler::onDisconnected);
    }

private:
    static QString contentType(const QString& path) {
        if (path.endsWith(".html", Qt::CaseInsensitive)) return "text/html; charset=utf-8";
        if (path.endsWith(".png", Qt::CaseInsensitive))  return "image/png";
        if (path.endsWith(".json", Qt::CaseInsensitive)) return "application/json";
        if (path.endsWith(".js", Qt::CaseInsensitive))   return "text/javascript";
        if (path.endsWith(".css", Qt::CaseInsensitive))  return "text/css";
        if (path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive)) return "image/jpeg";
        if (path.endsWith(".webp", Qt::CaseInsensitive)) return "image/webp";
        return "application/octet-stream";
    }

    void respond(int code, const QByteArray& ctype, const QByteArray& body) {
        QByteArray head = "HTTP/1.1 ";
        head += QByteArray::number(code);
        head += code == 200 ? " OK\r\n" : " ERR\r\n";
        head += "Content-Type: " + ctype + "\r\n";
        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
        sock_->write(head);
        sock_->write(body);
        sock_->disconnectFromHost();
    }

    void onReadyRead() {
        // 一次性读取请求 (浏览器 GET 一段完整请求)
        static thread_local QByteArray buf;
        buf.append(sock_->readAll());
        if (!buf.contains("\r\n\r\n")) return;
        const auto req = buf;
        buf.clear();

        const auto firstLineEnd = req.indexOf("\r\n");
        if (firstLineEnd < 0) { respond(400, "text/plain", "bad request"); return; }
        const auto firstLine = req.left(firstLineEnd);
        const auto parts = firstLine.split(' ');
        if (parts.size() < 2) { respond(400, "text/plain", "bad request"); return; }
        QString path = QString::fromUtf8(parts.at(1));
        const auto q = path.indexOf('?');
        if (q >= 0) path = path.left(q);
        if (path.startsWith('/')) path = path.mid(1);
        path = QUrl::fromPercentEncoding(path.toUtf8());
        // 路径安全
        if (path.contains("..")) { respond(403, "text/plain", "forbidden"); return; }
        QFileInfo fi(QDir(root_), path);
        QString abs;
        if (path.isEmpty() || path.endsWith('/')) {
            abs = fi.absoluteFilePath() + "/preview.html";
        } else {
            abs = fi.absoluteFilePath();
        }
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly)) {
            respond(404, "text/plain", "not found");
            return;
        }
        const auto data = f.readAll();
        respond(200, contentType(abs).toUtf8(), data);
    }

    void onDisconnected() {
        sock_->deleteLater();
        this->deleteLater();
    }

    QTcpSocket* sock_;
    QString root_;
};
} // anonymous

std::uint16_t PreviewServer::serve(const fs::path& dir) {
    if (!fs::is_directory(dir)) throw CoreError::invalid("dir not found: " + dir.string());
    auto& inst = instance();
    std::lock_guard<std::mutex> lk(inst.mu_);
    fs::path can = fs::canonical(dir);
    auto it = inst.entries_.find(can);
    if (it != inst.entries_.end()) return it->second.first;
    auto server = std::make_shared<QTcpServer>();
    if (!server->listen(QHostAddress::LocalHost, 0)) {
        throw CoreError::invalid("PreviewServer listen failed");
    }
    const auto port = static_cast<std::uint16_t>(server->serverPort());
    const auto rootStr = QString::fromStdString(can.string());
    QObject::connect(server.get(), &QTcpServer::newConnection, [server, rootStr]() {
        while (auto* sock = server->nextPendingConnection()) {
            new Handler(sock, rootStr);
        }
    });
    inst.entries_[can] = { port, server };
    return port;
}

} // namespace qcutter