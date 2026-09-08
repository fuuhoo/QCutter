#include "ui/main_window.h"
#include "state/app_state.h"
#include "ui/pages/home_shell.h"
#include "ui/app_theme.h"
#include "api/task_manager.h"
#include <QApplication>
#include <QCloseEvent>
#include <QStatusBar>
#include <QLabel>
#include <QTimer>

namespace qcutter {

MainWindow::MainWindow(std::shared_ptr<AppState> state, QWidget* parent)
    : QMainWindow(parent), state_(std::move(state)) {
    setWindowTitle("QCutter · TIFF 金字塔切片");
    resize(1280, 800);
    home_ = new HomeShell(state_, this);
    setCentralWidget(home_);
    statusBar()->showMessage("就绪");
}

MainWindow::~MainWindow() {
    TaskManager::instance().shutdown();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    TaskManager::instance().shutdown();
    QMainWindow::closeEvent(e);
}

} // namespace qcutter