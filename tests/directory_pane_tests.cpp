#include "ui/directory_pane.hpp"

#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

class directory_pane_tests final : public QObject {
    Q_OBJECT

private slots:
    void starts_without_a_directory();
    void setting_root_directory_populates_the_tree();
    void selected_paths_follow_tree_selection();
};

void directory_pane_tests::starts_without_a_directory() {
    ui::DirectoryPane pane;

    auto* tree = pane.findChild<QTreeView*>("directoryTree");
    QVERIFY(tree != nullptr);
    QVERIFY(!tree->isEnabled());
    QVERIFY(pane.rootDirectory().isEmpty());
}

void directory_pane_tests::setting_root_directory_populates_the_tree() {
    QTemporaryDir temporary_directory;
    QVERIFY(temporary_directory.isValid());

    ui::DirectoryPane pane;
    pane.setRootDirectory(temporary_directory.path());

    auto* tree = pane.findChild<QTreeView*>("directoryTree");
    QVERIFY(tree != nullptr);
    QVERIFY(tree->isEnabled());

    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);

    const auto expected = QDir(temporary_directory.path()).canonicalPath();
    QCOMPARE(pane.rootDirectory(), expected);
    QCOMPARE(QDir(model->filePath(tree->rootIndex())).canonicalPath(), expected);
}


void directory_pane_tests::selected_paths_follow_tree_selection() {
    QTemporaryDir temporary_directory;
    QVERIFY(temporary_directory.isValid());

    const auto first_path = temporary_directory.filePath(QStringLiteral("first.cpp"));
    const auto second_path = temporary_directory.filePath(QStringLiteral("second.hpp"));
    for (const auto& path : {first_path, second_path}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("x") == 1);
    }

    ui::DirectoryPane pane;
    pane.setRootDirectory(temporary_directory.path());

    auto* tree = pane.findChild<QTreeView*>("directoryTree");
    auto* zip_button = pane.findChild<QPushButton*>("zipAttachButton");
    QVERIFY(tree != nullptr);
    QVERIFY(zip_button != nullptr);
    QVERIFY(!zip_button->isEnabled());

    auto* model = qobject_cast<QFileSystemModel*>(tree->model());
    QVERIFY(model != nullptr);
    QTRY_VERIFY(model->index(first_path).isValid());
    QTRY_VERIFY(model->index(second_path).isValid());

    tree->selectionModel()->select(
        model->index(first_path), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    tree->selectionModel()->select(
        model->index(second_path), QItemSelectionModel::Select | QItemSelectionModel::Rows);

    QCOMPARE(pane.selectedPaths(), (QStringList{first_path, second_path}));
    QVERIFY(zip_button->isEnabled());
}

QTEST_MAIN(directory_pane_tests)
#include "directory_pane_tests.moc"
