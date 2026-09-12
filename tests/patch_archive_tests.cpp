#include "archive/patch_archive.hpp"

#include <QDir>
#include <QFile>
#include <QPair>
#include <QTemporaryDir>
#include <QTest>

#include <miniz.h>

namespace {

bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(contents) == contents.size();
}

bool createArchive(
    const QString& archive_path,
    const QList<QPair<QString, QByteArray>>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, archive_path.toUtf8().constData(), 0)) {
        return false;
    }

    bool success = true;
    for (const auto& [name, contents] : entries) {
        const auto archive_name = name.toUtf8();
        const void* data = contents.isEmpty() ? nullptr : contents.constData();
        if (!mz_zip_writer_add_mem(
                &zip,
                archive_name.constData(),
                data,
                static_cast<size_t>(contents.size()),
                MZ_DEFAULT_LEVEL)) {
            success = false;
            break;
        }
    }

    if (success) {
        success = mz_zip_writer_finalize_archive(&zip) != 0;
    }
    mz_zip_writer_end(&zip);
    return success;
}

const archive::PatchFile* findEntry(
    const archive::PatchInspection& inspection,
    const QString& relative_path) {
    for (const auto& entry : inspection.files) {
        if (entry.relative_path == relative_path) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

class patch_archive_tests final : public QObject {
    Q_OBJECT

private slots:
    void classifies_modified_added_and_unchanged_files();
    void rejects_parent_traversal();
    void rejects_case_insensitive_duplicate_paths();
    void rejects_file_directory_target_mismatch();
    void rejects_redundant_project_root_wrapper();
    void applies_modified_and_added_files();
    void refuses_to_apply_if_target_changed_after_inspection();
};

void patch_archive_tests::classifies_modified_added_and_unchanged_files() {
    QTemporaryDir project;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(output.isValid());
    QVERIFY(QDir(project.path()).mkpath(QStringLiteral("src")));
    QVERIFY(writeFile(project.filePath(QStringLiteral("src/modified.cpp")), QByteArrayLiteral("old\n")));
    QVERIFY(writeFile(project.filePath(QStringLiteral("src/same.cpp")), QByteArrayLiteral("same\n")));

    const auto archive_path = output.filePath(QStringLiteral("patch.zip"));
    QVERIFY(createArchive(
        archive_path,
        {
            {QStringLiteral("src/modified.cpp"), QByteArrayLiteral("new\n")},
            {QStringLiteral("src/same.cpp"), QByteArrayLiteral("same\n")},
            {QStringLiteral("src/new.cpp"), QByteArrayLiteral("added\n")},
        }));

    const auto inspection = archive::inspectPatchArchive(archive_path, project.path());
    QVERIFY2(inspection.success, qPrintable(inspection.error));
    QCOMPARE(inspection.files.size(), 3);

    const auto* modified = findEntry(inspection, QStringLiteral("src/modified.cpp"));
    const auto* same = findEntry(inspection, QStringLiteral("src/same.cpp"));
    const auto* added = findEntry(inspection, QStringLiteral("src/new.cpp"));
    QVERIFY(modified);
    QVERIFY(same);
    QVERIFY(added);
    QCOMPARE(modified->state, archive::PatchFileState::Modified);
    QCOMPARE(same->state, archive::PatchFileState::Unchanged);
    QCOMPARE(added->state, archive::PatchFileState::Added);
}

void patch_archive_tests::rejects_parent_traversal() {
    QTemporaryDir project;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(output.isValid());

    const auto archive_path = output.filePath(QStringLiteral("patch.zip"));
    QVERIFY(createArchive(
        archive_path,
        {{QStringLiteral("../outside.cpp"), QByteArrayLiteral("bad\n")}}));

    const auto inspection = archive::inspectPatchArchive(archive_path, project.path());
    QVERIFY(!inspection.success);
    QVERIFY(inspection.error.contains(QStringLiteral("unsafe"), Qt::CaseInsensitive));
}

void patch_archive_tests::rejects_case_insensitive_duplicate_paths() {
    QTemporaryDir project;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(output.isValid());

    const auto archive_path = output.filePath(QStringLiteral("patch.zip"));
    QVERIFY(createArchive(
        archive_path,
        {
            {QStringLiteral("src/Foo.cpp"), QByteArrayLiteral("one\n")},
            {QStringLiteral("src/foo.cpp"), QByteArrayLiteral("two\n")},
        }));

    const auto inspection = archive::inspectPatchArchive(archive_path, project.path());
    QVERIFY(!inspection.success);
    QVERIFY(inspection.error.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive));
}

void patch_archive_tests::rejects_file_directory_target_mismatch() {
    QTemporaryDir project;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(output.isValid());
    QVERIFY(QDir(project.path()).mkpath(QStringLiteral("src/widget.cpp")));

    const auto archive_path = output.filePath(QStringLiteral("patch.zip"));
    QVERIFY(createArchive(
        archive_path,
        {{QStringLiteral("src/widget.cpp"), QByteArrayLiteral("file contents\n")}}));

    const auto inspection = archive::inspectPatchArchive(archive_path, project.path());
    QVERIFY(!inspection.success);
    QVERIFY(inspection.error.contains(QStringLiteral("directory"), Qt::CaseInsensitive));
}

void patch_archive_tests::rejects_redundant_project_root_wrapper() {
    QTemporaryDir parent;
    QTemporaryDir output;
    QVERIFY(parent.isValid());
    QVERIFY(output.isValid());

    const auto project_path = parent.filePath(QStringLiteral("sample_project"));
    QVERIFY(QDir().mkpath(project_path + QStringLiteral("/src")));
    QVERIFY(writeFile(project_path + QStringLiteral("/src/existing.cpp"), QByteArrayLiteral("old\n")));

    const auto archive_path = output.filePath(QStringLiteral("patch.zip"));
    QVERIFY(createArchive(
        archive_path,
        {{QStringLiteral("sample_project/src/existing.cpp"), QByteArrayLiteral("new\n")}}));

    const auto inspection = archive::inspectPatchArchive(archive_path, project_path);
    QVERIFY(!inspection.success);
    QVERIFY(inspection.error.contains(QStringLiteral("project directory"), Qt::CaseInsensitive));
}

void patch_archive_tests::applies_modified_and_added_files() {
    QTemporaryDir project;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(output.isValid());
    QVERIFY(QDir(project.path()).mkpath(QStringLiteral("src")));
    QVERIFY(writeFile(project.filePath(QStringLiteral("src/modified.cpp")), QByteArrayLiteral("old\n")));

    const auto archive_path = output.filePath(QStringLiteral("patch.zip"));
    QVERIFY(createArchive(
        archive_path,
        {
            {QStringLiteral("src/modified.cpp"), QByteArrayLiteral("new\n")},
            {QStringLiteral("src/new/nested.hpp"), QByteArrayLiteral("added\n")},
        }));

    const auto inspection = archive::inspectPatchArchive(archive_path, project.path());
    QVERIFY2(inspection.success, qPrintable(inspection.error));

    const auto result = archive::applyPatch(project.path(), inspection);
    QVERIFY2(result.success, qPrintable(result.error));

    QFile modified(project.filePath(QStringLiteral("src/modified.cpp")));
    QVERIFY(modified.open(QIODevice::ReadOnly));
    QCOMPARE(modified.readAll(), QByteArrayLiteral("new\n"));

    QFile added(project.filePath(QStringLiteral("src/new/nested.hpp")));
    QVERIFY(added.open(QIODevice::ReadOnly));
    QCOMPARE(added.readAll(), QByteArrayLiteral("added\n"));
}


void patch_archive_tests::refuses_to_apply_if_target_changed_after_inspection() {
    QTemporaryDir project;
    QTemporaryDir output;
    QVERIFY(project.isValid());
    QVERIFY(output.isValid());
    QVERIFY(QDir(project.path()).mkpath(QStringLiteral("src")));

    const auto target_path = project.filePath(QStringLiteral("src/modified.cpp"));
    QVERIFY(writeFile(target_path, QByteArrayLiteral("original\n")));

    const auto archive_path = output.filePath(QStringLiteral("patch.zip"));
    QVERIFY(createArchive(
        archive_path,
        {{QStringLiteral("src/modified.cpp"), QByteArrayLiteral("from zip\n")}}));

    const auto inspection = archive::inspectPatchArchive(archive_path, project.path());
    QVERIFY2(inspection.success, qPrintable(inspection.error));

    QVERIFY(writeFile(target_path, QByteArrayLiteral("new local edit\n")));
    const auto result = archive::applyPatch(project.path(), inspection);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("changed since"), Qt::CaseInsensitive));

    QFile target(target_path);
    QVERIFY(target.open(QIODevice::ReadOnly));
    QCOMPARE(target.readAll(), QByteArrayLiteral("new local edit\n"));
}

QTEST_APPLESS_MAIN(patch_archive_tests)
#include "patch_archive_tests.moc"
