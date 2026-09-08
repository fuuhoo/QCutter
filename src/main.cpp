// QCutter 主入口.
#include "api/preview_server.h"
#include "api/task_manager.h"
#include "engine/alpha.h"
#include "engine/cutter.h"
#include "engine/meta.h"
#include "engine/source.h"
#include "state/app_state.h"
#include "ui/main_window.h"
#include "ui/app_theme.h"
#include "util/logger.h"
#include "util/paths.h"

#include <QApplication>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QImage>
#include <QTcpServer>

#include "util/fs_compat.h"
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    // 立即刷新 stderr 缓冲
    fflush(stderr);

    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, NULL, TRUE);

    // 抓当前调用栈
    void* stack[64] = {nullptr};
    USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);

    fprintf(stderr, "\n[QCutter] *** CRASH *** exception=0x%08lx addr=%p frames=%u\n",
            ep ? ep->ExceptionRecord->ExceptionCode : 0,
            ep ? ep->ExceptionRecord->ExceptionAddress : nullptr,
            frames);
    fflush(stderr);

    // 同时写日志文件
    auto log = qcutter::appDataDir() / "logs" / "crash.log";
    QDir().mkpath(QString::fromStdString((qcutter::appDataDir() / "logs").string()));
    FILE* f = fopen(log.string().c_str(), "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "\n=== CRASH %04d-%02d-%02d %02d:%02d:%02d ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "exception=0x%08lx addr=%p frames=%u\n",
                ep ? ep->ExceptionRecord->ExceptionCode : 0,
                ep ? ep->ExceptionRecord->ExceptionAddress : nullptr,
                frames);

        // 输出每一帧
        for (USHORT i = 0; i < frames; ++i) {
            DWORD64 addr = (DWORD64)stack[i];
            // 先 raw address (在没符号情况下也能定位)
            fprintf(f, "  #%02u: 0x%016llx", i, (unsigned long long)addr);

            // 尝试解析符号
            char symBuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 256;
            DWORD64 disp = 0;
            if (SymFromAddr(proc, addr, &disp, sym)) {
                fprintf(f, "  %s+0x%llx", sym->Name, (unsigned long long)disp);
            }
            // 尝试模块 + offset
            IMAGEHLP_MODULE64 mod = {};
            mod.SizeOfStruct = sizeof(mod);
            if (SymGetModuleInfo64(proc, addr, &mod)) {
                DWORD64 modBase = SymGetModuleBase64(proc, addr);
                if (modBase) {
                    fprintf(f, "  [%s+0x%llx]", mod.ModuleName,
                            (unsigned long long)(addr - modBase));
                }
            }
            fprintf(f, "\n");
        }
        fclose(f);
    }

    fprintf(stderr, "[QCutter] crash dump written to %s\n", log.string().c_str());
    fflush(stderr);

    SymCleanup(proc);
    return EXCEPTION_EXECUTE_HANDLER;  // 不弹窗
}

static void installCrashHandler() {
    SetUnhandledExceptionFilter(crashHandler);
    // SIGABRT 在 MinGW 用 _set_abort_behavior 处理更稳; 这里只设 SEH
}
#else
static void installCrashHandler() {}
#endif

// CLI 模式: 直接读取 TIFF 并生成预览 PNG.
//   qcutter.exe --cli <tiff> [<out.png>] [--max-px N] [--read-tiles N]
// 不启动 Qt UI, 仅使用 QCutterCore + libtiff.
static int runCli(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "--cli") != 0) {
        std::fprintf(stderr, "CLI usage: qcutter --cli <input.tiff> [output.png] [options]\n");
        std::fprintf(stderr, "  --max-px N       preview max dimension (default 720)\n");
        std::fprintf(stderr, "  --read-tiles N   decode N tiles then stop (default 200, 0=stop at first failure)\n");
        return 2;
    }
    QString input = QString::fromLocal8Bit(argv[2]);
    QString output = (argc >= 4 && std::strncmp(argv[3], "--", 2) != 0)
                         ? QString::fromLocal8Bit(argv[3])
                         : QString();
    std::uint32_t max_px = 720;
    std::uint32_t read_tiles = 200;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max-px") == 0 && i + 1 < argc) {
            max_px = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--read-tiles") == 0 && i + 1 < argc) {
            read_tiles = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        }
    }

    std::fprintf(stderr, "[cli] input=%s output=%s max_px=%u read_tiles=%u\n",
                 input.toStdString().c_str(),
                 output.toStdString().c_str(), max_px, read_tiles);

    // Step 1: readImageInfo
    std::fprintf(stderr, "[cli] step 1: readImageInfo\n");
    qcutter::ImageBrief brief;
    try {
        brief = qcutter::TaskManager::readImageInfo(input);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[cli] readImageInfo FAILED: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "[cli]   image %ux%u chunks=%u chunk=%ux%u compression=%s\n",
                 brief.width, brief.height, brief.max_level,
                 brief.chunk_w, brief.chunk_h,
                 brief.compression.toStdString().c_str());

    // Step 2: probeGeoref
    std::fprintf(stderr, "[cli] step 2: probeGeoref\n");
    try {
        auto geo = qcutter::probeGeoref(fs::path(input.toStdString()));
        if (geo) {
            std::fprintf(stderr, "[cli]   georef: mx0=%.3f my_top=%.3f sx=%.6f sy=%.6f epsg=%u\n",
                         geo->mx0, geo->my_top, geo->sx, geo->sy, geo->src_epsg);
        } else {
            std::fprintf(stderr, "[cli]   no georef\n");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[cli] probeGeoref FAILED: %s\n", e.what());
        return 1;
    }

    // Step 3: makePreview via TaskManager
    std::fprintf(stderr, "[cli] step 3: makePreview via TaskManager\n");
    QByteArray png;
    try {
        png = qcutter::TaskManager::makePreview(input, max_px, qcutter::AlphaMode::keep());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[cli] makePreview FAILED: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "[cli]   preview png size=%lld bytes\n", (long long)png.size());
    if (output.isEmpty()) {
        output = QFileInfo(input).absolutePath() + "/" + QFileInfo(input).completeBaseName() + "_preview.png";
    }
    QFile f(output);
    if (!f.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "[cli] cannot write %s\n", output.toStdString().c_str());
        return 1;
    }
    f.write(png);
    f.close();
    std::fprintf(stderr, "[cli]   preview written to %s\n", output.toStdString().c_str());

    // Step 4: optionally read more tiles to stress-test
    if (read_tiles > 0) {
        std::fprintf(stderr, "[cli] step 4: stress-decode first %u tiles\n", read_tiles);
        try {
            qcutter::SourceReader r(fs::path(input.toStdString()));
            for (std::uint32_t i = 0; i < read_tiles; ++i) {
                auto c = r.decodeChunkPublic(i);
                if (!c) {
                    std::fprintf(stderr, "[cli]   tile %u returned nullopt (EOF)\n", i);
                    break;
                }
                if (i % 50 == 0) {
                    std::fprintf(stderr, "[cli]   tile %u OK %ux%u rgba=%zu\n",
                                 i, c->w, c->h, c->rgba.size());
                }
            }
            std::fprintf(stderr, "[cli]   stress-decode OK\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[cli] stress-decode FAILED: %s\n", e.what());
            return 1;
        }
    }

    std::fprintf(stderr, "[cli] DONE\n");
    return 0;
}

// CLI 切片模式: 跑完整切片流水线, 输出 preview.html 并启动 HTTP 预览.
//   qcutter.exe --cut <tiff> <output-dir> [--tile-size N] [--zmin N] [--zmax N] [--keep-server]
// 不启动 UI.
static int runCutCli(int argc, char** argv,
                     std::shared_ptr<qcutter::AppState> state) {
    if (argc < 4 || std::strcmp(argv[1], "--cut") != 0) {
        std::fprintf(stderr, "cut usage: qcutter --cut <input.tiff> <output-dir> [options]\n");
        std::fprintf(stderr, "  --tile-size N     tile size (default 256)\n");
        std::fprintf(stderr, "  --zmin N          min zoom level (auto if absent)\n");
        std::fprintf(stderr, "  --zmax N          max zoom level (auto if absent)\n");
        std::fprintf(stderr, "  --mercator        use Web Mercator (XY/Z) coordinate system\n");
        std::fprintf(stderr, "  --keep-server     keep PreviewServer alive after cutting (default: serve and print URL, then exit)\n");
        return 2;
    }
    QString input = QString::fromLocal8Bit(argv[2]);
    QString outputDir = QString::fromLocal8Bit(argv[3]);

    qcutter::CutParams p;
    p.source = fs::path(input.toStdString());
    p.output = fs::path(outputDir.toStdString());
    bool keepServer = false;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tile-size") == 0 && i + 1 < argc) {
            p.tile_size = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--zmin") == 0 && i + 1 < argc) {
            p.zmin = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--zmax") == 0 && i + 1 < argc) {
            p.zmax = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--mercator") == 0) {
            p.mercator = true;
        } else if (std::strcmp(argv[i], "--keep-server") == 0) {
            keepServer = true;
        }
    }
    // 把全局设置中的底图 (含默认 OpenStreetMap) 传给 preview.html
    if (state) {
        p.preview_overlays = state->overlaysJson().toStdString();
    }

    std::fprintf(stderr, "[cut] input=%s output=%s tile_size=%u\n",
                 input.toStdString().c_str(),
                 outputDir.toStdString().c_str(),
                 p.tile_size);

    // Ensure output dir exists
    QDir().mkpath(outputDir);

    // Run cut
    std::fprintf(stderr, "[cut] running runCutWithControl...\n");
    qcutter::CutSummary summary;
    try {
        auto control = qcutter::TaskControl::create();
        qcutter::CutSink sink = [](const qcutter::CutEvent&) {};
        summary = qcutter::runCutWithControl(p, control, sink);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[cut] FAILED: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "[cut] total_tiles=%llu bytes=%llu elapsed_ms=%llu errors=%zu\n",
                 (unsigned long long)summary.total_tiles,
                 (unsigned long long)summary.bytes_written,
                 (unsigned long long)summary.elapsed_ms,
                 summary.errors.size());
    for (const auto& err : summary.errors) {
        std::fprintf(stderr, "[cut] error: %s\n", err.c_str());
    }

    // Start PreviewServer
    std::uint16_t port = 0;
    try {
        port = qcutter::PreviewServer::serve(p.output);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[cut] PreviewServer failed: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "[cut] preview.html  -> http://127.0.0.1:%u/preview.html\n", port);
    std::fprintf(stderr, "[cut] manifest.json -> http://127.0.0.1:%u/manifest.json\n", port);
    std::fprintf(stderr, "[cut] output dir    : %s\n", outputDir.toStdString().c_str());
    fs::path htmlPath = p.output / "preview.html";
    std::fprintf(stderr, "[cut] local html    : %s\n", htmlPath.string().c_str());

    if (keepServer) {
        // Block forever: run Qt event loop so the HTTP server keeps serving.
        QApplication::exec();
    } else {
        std::fprintf(stderr, "[cut] DONE\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    installCrashHandler();
    std::setvbuf(stderr, NULL, _IOLBF, 0);  // line-buffered stderr

    // CLI 模式不启动 UI. 必须在 QApplication 之前处理.
    if (argc >= 2 && std::strcmp(argv[1], "--cut") == 0) {
        QApplication app(argc, argv);
        QCoreApplication::setOrganizationName("QCutter");
        QCoreApplication::setOrganizationDomain("qcutter.app");
        QCoreApplication::setApplicationName("QCutter");
        QCoreApplication::setApplicationVersion("0.1.0");
        // 从 AppState 加载默认底图 (settings.json 中保存的). 这样 preview.html
        // 也会带 OpenStreetMap 等在线叠加层, 与 UI 流程一致.
        auto state_for_cli = std::make_shared<qcutter::AppState>();
        state_for_cli->loadSettings();
        int rc = runCutCli(argc, argv, state_for_cli);
        std::fflush(stderr);
        return rc;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--cli") == 0) {
        // QApplication 仍需要 (TaskManager 内部用了 QString), 但不显示任何 widget.
        QApplication app(argc, argv);
        QCoreApplication::setOrganizationName("QCutter");
        QCoreApplication::setOrganizationDomain("qcutter.app");
        QCoreApplication::setApplicationName("QCutter");
        QCoreApplication::setApplicationVersion("0.1.0");
        int rc = runCli(argc, argv);
        std::fflush(stderr);
        return rc;
    }

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("QCutter");
    QCoreApplication::setOrganizationDomain("qcutter.app");
    QCoreApplication::setApplicationName("QCutter");
    QCoreApplication::setApplicationVersion("0.1.0");

    auto state = std::make_shared<qcutter::AppState>();
    state->loadSettings();
    qcutter::AppTheme::apply(state->darkTheme());

    qcutter::TaskManager::instance().bootstrap();
    state->bootstrap();

    qcutter::MainWindow w(state);
    w.show();
    LOG_I("main", "QCutter started");

    return app.exec();
}
