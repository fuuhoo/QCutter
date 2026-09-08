// QCutter 主窗口.
#pragma once
#include <QMainWindow>
#include <memory>

namespace qcutter {

class AppState;
class HomeShell;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(std::shared_ptr<AppState> state, QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    std::shared_ptr<AppState> state_;
    HomeShell* home_ = nullptr;
};

} // namespace qcutter