#include "archive/patch_archive.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <limits>
#include <utility>

#include <miniz.h>

namespace archive {
namespace {

struct ReaderGuard final {
    explicit ReaderGuard(mz_zip_archive& archive)
        : archive_(archive) {}

    ReaderGuard(const ReaderGuard&) = delete;
    ReaderGuard& operator=(const ReaderGuard&) = delete;

    ~ReaderGuard() {
        if (initialized_) {
            mz_zip_reader_end(&archive_);
        }
    }

    void markInitialized() { initialized_ = true; }

private:
    mz_zip_archive& archive_;
    bool initialized_{};
};

[[nodiscard]] PatchInspection inspectionFailure(QString error) {
    return {.success = false, .error = std::move(error)};
}

[[nodiscard]] PatchResult applyFailure(QString error) {
    return {.success = false, .error = std::move(error)};
}

[[nodiscard]] QString minizError(mz_zip_archive& archive) {
    const auto error = mz_zip_peek_last_error(&archive);
    const auto* text = mz_zip_get_error_string(error);
    return text ? QString::fromLatin1(text) : QStringLiteral("unknown miniz error");
}

[[nodiscard]] QString canonicalPath(const QFileInfo& info) {
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

[[nodiscard]] bool isLinkLike(const QFileInfo& info) {
    return info.isSymLink() || info.isJunction();
}

[[nodiscard]] bool isReservedWindowsName(const QString& component) {
    auto base = component.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> fixed_names{
        QStringLiteral("CON"),
        QStringLiteral("PRN"),
        QStringLiteral("AUX"),
        QStringLiteral("NUL"),
    };
    if (fixed_names.contains(base)) {
        return true;
    }

    if (base.size() == 4) {
        const auto prefix = base.left(3);
        const auto digit = base.at(3);
        if ((prefix == QStringLiteral("COM") || prefix == QStringLiteral("LPT")) &&
            digit >= QLatin1Char('1') && digit <= QLatin1Char('9')) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool normalizeArchivePath(
    QString path,
    bool is_directory,
    QString& normalized,
    QString& error) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (is_directory && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }

    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) ||
        (path.size() >= 2 && path.at(1) == QLatin1Char(':'))) {
        error = QStringLiteral("ZIP contains an unsafe path: %1").arg(path);
        return false;
    }

    const auto components = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const auto& component : components) {
        if (component.isEmpty() || component == QStringLiteral(".") ||
            component == QStringLiteral("..")) {
            error = QStringLiteral("ZIP contains an unsafe path: %1").arg(path);
            return false;
        }
        if (component.endsWith(QLatin1Char(' ')) || component.endsWith(QLatin1Char('.')) ||
            isReservedWindowsName(component)) {
            error = QStringLiteral("ZIP contains a path that is unsafe on Windows: %1").arg(path);
            return false;
        }
        for (const auto ch : component) {
            if (ch.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(ch)) {
                error = QStringLiteral("ZIP contains a path that is unsafe on Windows: %1").arg(path);
                return false;
            }
        }
    }

    normalized = components.join(QLatin1Char('/'));
    return true;
}

[[nodiscard]] bool validateTargetPath(
    const QString& root,
    const QString& relative_path,
    QString& error) {
    const auto components = relative_path.split(QLatin1Char('/'));
    auto current = root;

    for (qsizetype index = 0; index + 1 < components.size(); ++index) {
        current = QDir(current).filePath(components.at(index));
        const QFileInfo info(current);
        if (isLinkLike(info)) {
            error = QStringLiteral("Target path passes through a symbolic link or junction: %1")
                        .arg(relative_path);
            return false;
        }
        if (info.exists() && !info.isDir()) {
            error = QStringLiteral("ZIP expects a directory where the target contains a file: %1")
                        .arg(QDir(root).relativeFilePath(current));
            return false;
        }
    }

    const auto target_path = QDir(root).filePath(relative_path);
    const QFileInfo target_info(target_path);
    if (isLinkLike(target_info)) {
        error = QStringLiteral("ZIP would replace a symbolic link or junction: %1").arg(relative_path);
        return false;
    }
    if (target_info.exists() && target_info.isDir()) {
        error = QStringLiteral("ZIP contains a file where the target contains a directory: %1")
                    .arg(relative_path);
        return false;
    }
    if (target_info.exists() && !target_info.isFile()) {
        error = QStringLiteral("ZIP target is not a regular file: %1").arg(relative_path);
        return false;
    }

    return true;
}

[[nodiscard]] bool isUnsupportedUnixEntry(const mz_zip_archive_file_stat& stat) {
    constexpr mz_uint16 unix_host = 3;
    constexpr mz_uint32 file_type_mask = 0170000;
    constexpr mz_uint32 regular_file = 0100000;
    constexpr mz_uint32 directory = 0040000;

    if ((stat.m_version_made_by >> 8) != unix_host) {
        return false;
    }

    const auto mode = stat.m_external_attr >> 16;
    const auto type = mode & file_type_mask;
    return type != 0 && type != regular_file && type != directory;
}

[[nodiscard]] QString entryName(mz_zip_archive& zip, mz_uint index) {
    const auto required = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
    if (required == 0) {
        return {};
    }

    QByteArray buffer(static_cast<qsizetype>(required), '\0');
    if (mz_zip_reader_get_filename(&zip, index, buffer.data(), required) == 0) {
        return {};
    }
    return QString::fromUtf8(buffer.constData());
}

[[nodiscard]] bool looksLikeRedundantProjectWrapper(
    const QString& root,
    const QList<PatchFile>& files) {
    if (files.isEmpty()) {
        return false;
    }

    const auto project_name = QFileInfo(root).fileName();
    if (project_name.isEmpty()) {
        return false;
    }

    bool stripped_path_exists = false;
    for (const auto& file : files) {
        const auto slash = file.relative_path.indexOf(QLatin1Char('/'));
        if (slash <= 0 ||
            file.relative_path.left(slash).compare(project_name, Qt::CaseInsensitive) != 0) {
            return false;
        }

        const auto stripped = file.relative_path.mid(slash + 1);
        if (QFileInfo(QDir(root).filePath(stripped)).exists()) {
            stripped_path_exists = true;
        }
    }

    return stripped_path_exists;
}

} // namespace

PatchInspection inspectPatchArchive(
    const QString& archive_path,
    const QString& target_root) {
    const QFileInfo root_info(target_root);
    if (!root_info.exists() || !root_info.isDir()) {
        return inspectionFailure(QStringLiteral("The target project directory is not valid."));
    }
    const auto root = canonicalPath(root_info);

    QFile archive_file(archive_path);
    if (!archive_file.open(QIODevice::ReadOnly)) {
        return inspectionFailure(
            QStringLiteral("Could not open downloaded ZIP: %1").arg(archive_file.errorString()));
    }
    const auto bytes = archive_file.readAll();
    if (archive_file.error() != QFileDevice::NoError) {
        return inspectionFailure(
            QStringLiteral("Could not read downloaded ZIP: %1").arg(archive_file.errorString()));
    }

    mz_zip_archive zip{};
    ReaderGuard reader_guard(zip);
    if (!mz_zip_reader_init_mem(
            &zip,
            bytes.isEmpty() ? nullptr : bytes.constData(),
            static_cast<size_t>(bytes.size()),
            0)) {
        return inspectionFailure(QStringLiteral("Downloaded file is not a readable ZIP: %1")
                                     .arg(minizError(zip)));
    }
    reader_guard.markInitialized();

    QHash<QString, bool> entry_kinds;
    QList<PatchFile> files;
    const auto count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat)) {
            return inspectionFailure(
                QStringLiteral("Could not inspect ZIP entry: %1").arg(minizError(zip)));
        }
        if (stat.m_is_encrypted || !stat.m_is_supported || isUnsupportedUnixEntry(stat)) {
            return inspectionFailure(
                QStringLiteral("ZIP contains an unsupported or unsafe entry: %1")
                    .arg(entryName(zip, index)));
        }

        auto name = entryName(zip, index);
        if (name.isEmpty()) {
            return inspectionFailure(QStringLiteral("ZIP contains an entry with no usable name."));
        }

        QString normalized;
        QString path_error;
        if (!normalizeArchivePath(name, stat.m_is_directory, normalized, path_error)) {
            return inspectionFailure(path_error);
        }

        const auto key = normalized.toCaseFolded();
        if (entry_kinds.contains(key)) {
            return inspectionFailure(
                QStringLiteral("ZIP contains duplicate or case-colliding paths: %1").arg(normalized));
        }
        entry_kinds.insert(key, stat.m_is_directory != 0);

        if (stat.m_is_directory) {
            continue;
        }

        const auto components = normalized.split(QLatin1Char('/'));
        QString parent;
        for (qsizetype component_index = 0; component_index + 1 < components.size(); ++component_index) {
            if (!parent.isEmpty()) {
                parent += QLatin1Char('/');
            }
            parent += components.at(component_index);
            const auto parent_key = parent.toCaseFolded();
            if (entry_kinds.contains(parent_key) && !entry_kinds.value(parent_key)) {
                return inspectionFailure(
                    QStringLiteral("ZIP contains a file/directory path conflict: %1").arg(normalized));
            }
        }

        QString target_error;
        if (!validateTargetPath(root, normalized, target_error)) {
            return inspectionFailure(target_error);
        }

        if (stat.m_uncomp_size > static_cast<mz_uint64>(std::numeric_limits<qsizetype>::max())) {
            return inspectionFailure(QStringLiteral("ZIP entry is too large to inspect: %1").arg(normalized));
        }

        QByteArray contents;
        contents.resize(static_cast<qsizetype>(stat.m_uncomp_size));
        if (!contents.isEmpty() &&
            !mz_zip_reader_extract_to_mem(
                &zip,
                index,
                contents.data(),
                static_cast<size_t>(contents.size()),
                0)) {
            return inspectionFailure(
                QStringLiteral("Could not extract ZIP entry %1: %2").arg(normalized, minizError(zip)));
        }

        PatchFileState state = PatchFileState::Added;
        bool target_existed = false;
        QByteArray original_contents;
        const auto target_path = QDir(root).filePath(normalized);
        const QFileInfo target_info(target_path);
        if (target_info.exists()) {
            QFile target(target_path);
            if (!target.open(QIODevice::ReadOnly)) {
                return inspectionFailure(
                    QStringLiteral("Could not read target file %1: %2")
                        .arg(normalized, target.errorString()));
            }
            original_contents = target.readAll();
            if (target.error() != QFileDevice::NoError) {
                return inspectionFailure(
                    QStringLiteral("Could not read target file %1: %2")
                        .arg(normalized, target.errorString()));
            }
            target_existed = true;
            if (original_contents == contents) {
                state = PatchFileState::Unchanged;
                original_contents = contents;
            } else {
                state = PatchFileState::Modified;
            }
        }

        files.append(
            {normalized, state, std::move(contents), target_existed, std::move(original_contents)});
    }

    if (files.isEmpty()) {
        return inspectionFailure(QStringLiteral("Downloaded ZIP does not contain any files."));
    }

    // Check file/parent conflicts again after all entries are known because ZIP entry
    // order is arbitrary (for example, "a/b.cpp" may appear before a file named "a").
    for (const auto& file : files) {
        const auto components = file.relative_path.split(QLatin1Char('/'));
        QString parent;
        for (qsizetype component_index = 0; component_index + 1 < components.size(); ++component_index) {
            if (!parent.isEmpty()) {
                parent += QLatin1Char('/');
            }
            parent += components.at(component_index);
            const auto parent_key = parent.toCaseFolded();
            if (entry_kinds.contains(parent_key) && !entry_kinds.value(parent_key)) {
                return inspectionFailure(
                    QStringLiteral("ZIP contains a file/directory path conflict: %1")
                        .arg(file.relative_path));
            }
        }
    }

    // Catch the common mismatch where a response ZIP contains "project-name/..."
    // even though target_root is already that project directory.
    if (looksLikeRedundantProjectWrapper(root, files)) {
        return inspectionFailure(
            QStringLiteral("ZIP appears to include the project directory itself. "
                           "Its paths do not match the selected target directory."));
    }

    // Entry order is irrelevant; stable sorting makes the review deterministic.
    std::sort(
        files.begin(),
        files.end(),
        [](const PatchFile& left, const PatchFile& right) {
            return left.relative_path < right.relative_path;
        });
    return {.success = true, .files = std::move(files)};
}

PatchResult applyPatch(
    const QString& target_root,
    const PatchInspection& inspection) {
    if (!inspection.success) {
        return applyFailure(QStringLiteral("Cannot apply a ZIP that did not pass validation."));
    }

    const QFileInfo root_info(target_root);
    if (!root_info.exists() || !root_info.isDir()) {
        return applyFailure(QStringLiteral("The target project directory is no longer valid."));
    }
    const auto root = canonicalPath(root_info);

    // Re-run target validation and compare the target with the state that was reviewed.
    // This prevents a local edit made while the confirmation dialog is open from being lost.
    for (const auto& file : inspection.files) {
        QString target_error;
        if (!validateTargetPath(root, file.relative_path, target_error)) {
            return applyFailure(target_error);
        }

        const auto target_path = QDir(root).filePath(file.relative_path);
        const QFileInfo target_info(target_path);
        if (!file.target_existed) {
            if (target_info.exists() || isLinkLike(target_info)) {
                return applyFailure(
                    QStringLiteral("Target changed since it was reviewed: %1")
                        .arg(file.relative_path));
            }
            continue;
        }

        if (!target_info.exists() || !target_info.isFile()) {
            return applyFailure(
                QStringLiteral("Target changed since it was reviewed: %1")
                    .arg(file.relative_path));
        }

        QFile target(target_path);
        if (!target.open(QIODevice::ReadOnly)) {
            return applyFailure(
                QStringLiteral("Could not re-read target file %1: %2")
                    .arg(file.relative_path, target.errorString()));
        }
        const auto current_contents = target.readAll();
        if (target.error() != QFileDevice::NoError) {
            return applyFailure(
                QStringLiteral("Could not re-read target file %1: %2")
                    .arg(file.relative_path, target.errorString()));
        }
        if (current_contents != file.original_contents) {
            return applyFailure(
                QStringLiteral("Target changed since it was reviewed: %1")
                    .arg(file.relative_path));
        }
    }

    for (const auto& file : inspection.files) {
        if (file.state == PatchFileState::Unchanged) {
            continue;
        }

        const auto target_path = QDir(root).filePath(file.relative_path);
        const auto parent_path = QFileInfo(target_path).absolutePath();
        if (!QDir().mkpath(parent_path)) {
            return applyFailure(
                QStringLiteral("Could not create target directory for: %1").arg(file.relative_path));
        }

        QFile::Permissions existing_permissions{};
        const QFileInfo existing_info(target_path);
        if (existing_info.exists()) {
            existing_permissions = existing_info.permissions();
        }

        QSaveFile output(target_path);
        if (!output.open(QIODevice::WriteOnly)) {
            return applyFailure(
                QStringLiteral("Could not open %1 for replacement: %2")
                    .arg(file.relative_path, output.errorString()));
        }
        if (existing_info.exists()) {
            output.setPermissions(existing_permissions);
        }
        if (output.write(file.contents) != file.contents.size()) {
            return applyFailure(
                QStringLiteral("Could not write %1: %2")
                    .arg(file.relative_path, output.errorString()));
        }
        if (!output.commit()) {
            return applyFailure(
                QStringLiteral("Could not finish replacing %1: %2")
                    .arg(file.relative_path, output.errorString()));
        }
    }

    return {.success = true};
}

} // namespace archive
