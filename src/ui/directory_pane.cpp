#include "ui/directory_pane.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QLabel>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace ui {

DirectoryPane::DirectoryPane(QWidget* parent)
    : QWidget(parent),
      model_(new QFileSystemModel(this)),
      tree_(new QTreeView(this)),
      root_label_(new QLabel(tr("No directory selected"), this)),
      zip_attach_button_(new QPushButton(tr("Zip && Attach"), this)) {
    auto* open_button = new QPushButton(tr("Open Directory..."), this);
    open_button->setObjectName("openDirectoryButton");
    zip_attach_button_->setObjectName("zipAttachButton");
    zip_attach_button_->setEnabled(false);

    root_label_->setObjectName("rootDirectoryLabel");
    root_label_->setWordWrap(true);
    root_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    model_->setReadOnly(true);

    tree_->setObjectName("directoryTree");
    tree_->setModel(model_);
    tree_->setEnabled(false);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(zip_attach_button_);
    layout->addWidget(root_label_);
    layout->addWidget(tree_, 1);
    layout->addWidget(open_button);

    connect(open_button, &QPushButton::clicked, this, [this] { chooseDirectory(); });
    connect(zip_attach_button_, &QPushButton::clicked, this, &DirectoryPane::zipAttachRequested);
    connect(
        tree_->selectionModel(),
        &QItemSelectionModel::selectionChanged,
        this,
        [this] { updateZipButton(); });
}

void DirectoryPane::setRootDirectory(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        return;
    }

    auto resolved_path = info.canonicalFilePath();
    if (resolved_path.isEmpty()) {
        resolved_path = info.absoluteFilePath();
    }

    root_directory_ = QDir::cleanPath(resolved_path);
    const auto root_index = model_->setRootPath(root_directory_);
    tree_->setRootIndex(root_index);
    tree_->clearSelection();
    tree_->setEnabled(true);
    root_label_->setText(root_directory_);
    updateZipButton();
}

QString DirectoryPane::rootDirectory() const {
    return root_directory_;
}

QStringList DirectoryPane::selectedPaths() const {
    QStringList paths;
    if (!tree_->selectionModel()) {
        return paths;
    }

    for (const auto& index : tree_->selectionModel()->selectedRows(0)) {
        const auto path = model_->filePath(index);
        if (!path.isEmpty()) {
            paths.append(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        }
    }
    paths.sort(Qt::CaseInsensitive);
    return paths;
}

void DirectoryPane::chooseDirectory() {
    const auto start_directory = root_directory_.isEmpty() ? QDir::homePath() : root_directory_;
    const auto directory = QFileDialog::getExistingDirectory(
        this,
        tr("Select Source Directory"),
        start_directory,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!directory.isEmpty()) {
        setRootDirectory(directory);
    }
}

void DirectoryPane::updateZipButton() {
    zip_attach_button_->setEnabled(!root_directory_.isEmpty() && !selectedPaths().isEmpty());
}

} // namespace ui
