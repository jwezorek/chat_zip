#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace archive {

enum class PatchFileState {
    Added,
    Modified,
    Unchanged,
};

struct PatchFile {
    QString relative_path;
    PatchFileState state{};
    QByteArray contents;
    bool target_existed{};
    QByteArray original_contents;
};

struct PatchInspection {
    bool success{};
    QString error;
    QList<PatchFile> files;
};

struct PatchResult {
    bool success{};
    QString error;
};

[[nodiscard]] PatchInspection inspectPatchArchive(
    const QString& archive_path,
    const QString& target_root);

[[nodiscard]] PatchResult applyPatch(
    const QString& target_root,
    const PatchInspection& inspection);

} // namespace archive
