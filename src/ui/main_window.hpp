#pragma once

#include <QMainWindow>

class QWebEngineProfile;

namespace ui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWebEngineProfile& web_profile, QWidget* parent = nullptr);

private:
    bool operation_in_progress_{};
};

} // namespace ui
