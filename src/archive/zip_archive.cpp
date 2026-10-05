#include "archive/zip_archive.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <miniz.h>

namespace archive {
namespace {

struct ArchiveItem {
    QString source_path;
    QString archive_path;
    qint64 source_size{};
};

void logZip(const QString& message) {
    qInfo().noquote()
        << QStringLiteral("%1 [ChatZip] %2")
               .arg(
                   QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                   message);
}

[[nodiscard]] ZipResult failure(QString message) {
    qWarning().noquote()
        << QStringLiteral("%1 [ChatZip] ZIP failed: %2")
               .arg(
                   QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                   message);
    return {.success = false, .error = std::move(message)};
}

[[nodiscard]] QString canonicalPath(const QFileInfo& info) {
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

[[nodiscard]] bool isInsideRoot(const QString& root, const QString& path) {
    const auto relative = QDir(root).relativeFilePath(path);
    return relative == QStringLiteral(".") ||
           (!QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
            !relative.startsWith(QStringLiteral("../")));
}

[[nodiscard]] QString minizError(mz_zip_archive& archive) {
    const auto error = mz_zip_peek_last_error(&archive);
    const auto* text = mz_zip_get_error_string(error);
    return text ? QString::fromLatin1(text) : QStringLiteral("unknown miniz error");
}

class WriterGuard final {
public:
    explicit WriterGuard(mz_zip_archive& archive)
        : archive_(archive) {}

    WriterGuard(const WriterGuard&) = delete;
    WriterGuard& operator=(const WriterGuard&) = delete;

    ~WriterGuard() {
        if (initialized_) {
            mz_zip_writer_end(&archive_);
        }
    }

    void markInitialized() { initialized_ = true; }

private:
    mz_zip_archive& archive_;
    bool initialized_{};
};

struct MinizFree {
    void operator()(void* pointer) const { mz_free(pointer); }
};

[[nodiscard]] ZipResult collectFile(
    const QString& root,
    const QString& source_path,
    QSet<QString>& seen_entries,
    std::vector<ArchiveItem>& items) {
    const QFileInfo source_info(source_path);
    if (!source_info.exists()) {
        return failure(QStringLiteral("Selected item no longer exists: %1").arg(source_path));
    }

    const auto canonical_source = canonicalPath(source_info);
    if (!isInsideRoot(root, canonical_source)) {
        return failure(
            QStringLiteral("Selected item is outside the project directory: %1").arg(source_path));
    }

    if (!source_info.isFile()) {
        return failure(QStringLiteral("Selected item is not a regular file: %1").arg(source_path));
    }

    auto archive_entry = QDir::fromNativeSeparators(QDir(root).relativeFilePath(canonical_source));
    if (archive_entry.isEmpty() || archive_entry == QStringLiteral(".")) {
        return failure(QStringLiteral("Could not determine relative path for: %1").arg(source_path));
    }

    if (!seen_entries.contains(archive_entry)) {
        seen_entries.insert(archive_entry);
        items.push_back({canonical_source, std::move(archive_entry), source_info.size()});
    }
    return {.success = true};
}

[[nodiscard]] ZipResult collectItems(
    const QString& root,
    const QStringList& selected_paths,
    std::vector<ArchiveItem>& items) {
    QSet<QString> seen_entries;

    for (const auto& selected_path : selected_paths) {
        const QFileInfo selected_info(selected_path);
        if (!selected_info.exists()) {
            return failure(QStringLiteral("Selected item no longer exists: %1").arg(selected_path));
        }

        const auto canonical_selected = canonicalPath(selected_info);
        if (!isInsideRoot(root, canonical_selected)) {
            return failure(
                QStringLiteral("Selected item is outside the project directory: %1").arg(selected_path));
        }

        if (selected_info.isFile()) {
            const auto result = collectFile(root, canonical_selected, seen_entries, items);
            if (!result.success) {
                return result;
            }
            continue;
        }

        if (!selected_info.isDir()) {
            return failure(QStringLiteral("Selected item cannot be archived: %1").arg(selected_path));
        }

        QDirIterator iterator(
            canonical_selected,
            QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const auto result = collectFile(root, iterator.next(), seen_entries, items);
            if (!result.success) {
                return result;
            }
        }
    }

    std::ranges::sort(items, {}, &ArchiveItem::archive_path);
    return {.success = true};
}

} // namespace

ZipResult createZipArchive(
    const QString& root_directory,
    const QStringList& selected_paths,
    const QString& archive_path) {
    QElapsedTimer total_timer;
    total_timer.start();

    if (selected_paths.isEmpty()) {
        return failure(QStringLiteral("No files or directories are selected."));
    }

    const QFileInfo root_info(root_directory);
    if (!root_info.exists() || !root_info.isDir()) {
        return failure(QStringLiteral("The project directory is not valid."));
    }
    const auto root = canonicalPath(root_info);

    logZip(QStringLiteral("Enumerating ZIP selection..."));
    QElapsedTimer phase_timer;
    phase_timer.start();
    std::vector<ArchiveItem> items;
    const auto collection_result = collectItems(root, selected_paths, items);
    if (!collection_result.success) {
        return collection_result;
    }
    if (items.empty()) {
        return failure(QStringLiteral("The selection does not contain any files."));
    }

    qint64 total_source_bytes = 0;
    for (const auto& item : items) {
        if (item.source_size > 0 &&
            total_source_bytes <= std::numeric_limits<qint64>::max() - item.source_size) {
            total_source_bytes += item.source_size;
        }
    }
    logZip(
        QStringLiteral("Enumeration complete: %1 file(s), %2 bytes, %3 ms")
            .arg(static_cast<qulonglong>(items.size()))
            .arg(total_source_bytes)
            .arg(phase_timer.elapsed()));

    mz_zip_archive zip{};
    WriterGuard writer_guard(zip);
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        return failure(QStringLiteral("Could not start ZIP writer: %1").arg(minizError(zip)));
    }
    writer_guard.markInitialized();

    phase_timer.restart();
    logZip(QStringLiteral("Reading and compressing ZIP contents..."));
    qsizetype processed = 0;
    for (const auto& item : items) {
        QFile source(item.source_path);
        if (!source.open(QIODevice::ReadOnly)) {
            return failure(
                QStringLiteral("Could not read %1: %2").arg(item.source_path, source.errorString()));
        }
        const auto contents = source.readAll();
        if (source.error() != QFileDevice::NoError) {
            return failure(
                QStringLiteral("Could not read %1: %2").arg(item.source_path, source.errorString()));
        }

        const auto archive_name = item.archive_path.toUtf8();
        const void* data = contents.isEmpty() ? nullptr : contents.constData();
        if (!mz_zip_writer_add_mem(
                &zip,
                archive_name.constData(),
                data,
                static_cast<size_t>(contents.size()),
                MZ_DEFAULT_LEVEL)) {
            return failure(
                QStringLiteral("Could not add %1 to ZIP: %2")
                    .arg(item.archive_path, minizError(zip)));
        }

        ++processed;
        if (processed == static_cast<qsizetype>(items.size()) || processed % 250 == 0) {
            logZip(
                QStringLiteral("Compressed %1/%2 file(s)")
                    .arg(processed)
                    .arg(static_cast<qulonglong>(items.size())));
        }
    }
    logZip(QStringLiteral("Read/compress complete: %1 ms").arg(phase_timer.elapsed()));

    phase_timer.restart();
    logZip(QStringLiteral("Finalizing and writing ZIP..."));
    void* archive_data_raw = nullptr;
    size_t archive_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &archive_data_raw, &archive_size)) {
        return failure(QStringLiteral("Could not finalize ZIP: %1").arg(minizError(zip)));
    }
    std::unique_ptr<void, MinizFree> archive_data(archive_data_raw);

    if (archive_size > static_cast<size_t>(std::numeric_limits<qint64>::max())) {
        return failure(QStringLiteral("ZIP archive is too large to write."));
    }

    QSaveFile output(archive_path);
    if (!output.open(QIODevice::WriteOnly)) {
        return failure(
            QStringLiteral("Could not create %1: %2").arg(archive_path, output.errorString()));
    }

    const auto bytes_to_write = static_cast<qint64>(archive_size);
    if (output.write(static_cast<const char*>(archive_data.get()), bytes_to_write) != bytes_to_write) {
        return failure(
            QStringLiteral("Could not write %1: %2").arg(archive_path, output.errorString()));
    }
    if (!output.commit()) {
        return failure(
            QStringLiteral("Could not finish %1: %2").arg(archive_path, output.errorString()));
    }

    logZip(
        QStringLiteral("ZIP complete: %1 bytes, write/finalize %2 ms, total %3 ms")
            .arg(static_cast<qulonglong>(archive_size))
            .arg(phase_timer.elapsed())
            .arg(total_timer.elapsed()));
    return {.success = true};
}

} // namespace archive
