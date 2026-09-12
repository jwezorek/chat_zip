#pragma once

#include <QString>
#include <QStringList>

namespace archive {

struct ZipResult {
    bool success{};
    QString error;
};

[[nodiscard]] ZipResult createZipArchive(
    const QString& root_directory,
    const QStringList& selected_paths,
    const QString& archive_path);

} // namespace archive
