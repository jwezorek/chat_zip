#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QFileSystemModel;
class QLabel;
class QPoint;
class QPushButton;
class QTreeView;

namespace ui {

class DirectoryPane final : public QWidget {
    Q_OBJECT

public:
    explicit DirectoryPane(QWidget* parent = nullptr);

    void setRootDirectory(const QString& path);
    [[nodiscard]] QString rootDirectory() const;
    [[nodiscard]] QStringList selectedPaths() const;

signals:
    void zipAttachRequested();
    void fileAttachRequested(const QString& path);

private:
    void chooseDirectory();
    void showContextMenu(const QPoint& position);
    void updateZipButton();

    QFileSystemModel* model_{};
    QTreeView* tree_{};
    QLabel* root_label_{};
    QPushButton* zip_attach_button_{};
    QString root_directory_;
};

} // namespace ui
