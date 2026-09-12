#include "archive/zip_archive.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <miniz.h>

namespace {

bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(contents) == contents.size();
}

QStringList archiveEntries(const QString& archive_path) {
    QFile file(archive_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const auto bytes = file.readAll();
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, bytes.constData(), static_cast<size_t>(bytes.size()), 0)) {
        return {};
    }

    QStringList entries;
    const auto count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&archive, index, &stat)) {
            entries.append(QString::fromUtf8(stat.m_filename));
        }
    }
    mz_zip_reader_end(&archive);
    entries.sort(Qt::CaseSensitive);
    return entries;
}

} // namespace

class zip_archive_tests final : public QObject {
    Q_OBJECT

private slots:
    void selected_file_keeps_project_relative_path();
    void selected_directory_is_recursive_and_deduplicated();
    void rejects_items_outside_project_root();
};

void zip_archive_tests::selected_file_keeps_project_relative_path() {
    QTemporaryDir project;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(output.isValid());
    QVERIFY(QDir(project.path()).mkpath(QStringLiteral("src")));

    const auto source_path = project.filePath(QStringLiteral("src/example.cpp"));
    QVERIFY(writeFile(source_path, QByteArrayLiteral("int answer = 42;\n")));

    const auto archive_path = output.filePath(QStringLiteral("selection.zip"));
    const auto result = archive::createZipArchive(project.path(), {source_path}, archive_path);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(archiveEntries(archive_path), QStringList{QStringLiteral("src/example.cpp")});
}

void zip_archive_tests::selected_directory_is_recursive_and_deduplicated() {
    QTemporaryDir project;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(output.isValid());
    QVERIFY(QDir(project.path()).mkpath(QStringLiteral("src/nested")));

    const auto first_path = project.filePath(QStringLiteral("src/a.cpp"));
    const auto second_path = project.filePath(QStringLiteral("src/nested/b.hpp"));
    QVERIFY(writeFile(first_path, QByteArrayLiteral("a")));
    QVERIFY(writeFile(second_path, QByteArrayLiteral("b")));

    const auto archive_path = output.filePath(QStringLiteral("selection.zip"));
    const auto source_directory = project.filePath(QStringLiteral("src"));
    const auto result = archive::createZipArchive(
        project.path(), {source_directory, first_path}, archive_path);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(
        archiveEntries(archive_path),
        (QStringList{QStringLiteral("src/a.cpp"), QStringLiteral("src/nested/b.hpp")}));
}

void zip_archive_tests::rejects_items_outside_project_root() {
    QTemporaryDir project;
    QTemporaryDir outside;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(outside.isValid());
    QVERIFY(output.isValid());

    const auto outside_path = outside.filePath(QStringLiteral("secret.txt"));
    QVERIFY(writeFile(outside_path, QByteArrayLiteral("nope")));

    const auto archive_path = output.filePath(QStringLiteral("selection.zip"));
    const auto result = archive::createZipArchive(project.path(), {outside_path}, archive_path);

    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!QFile::exists(archive_path));
}

QTEST_APPLESS_MAIN(zip_archive_tests)
#include "zip_archive_tests.moc"
