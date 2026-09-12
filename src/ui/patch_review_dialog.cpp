#include "ui/patch_review_dialog.hpp"

#include "archive/patch_archive.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace ui {
namespace {

[[nodiscard]] QString reviewText(const archive::PatchInspection& inspection) {
    QStringList modified;
    QStringList added;
    QStringList unchanged;

    for (const auto& file : inspection.files) {
        switch (file.state) {
        case archive::PatchFileState::Modified:
            modified.append(file.relative_path);
            break;
        case archive::PatchFileState::Added:
            added.append(file.relative_path);
            break;
        case archive::PatchFileState::Unchanged:
            unchanged.append(file.relative_path);
            break;
        }
    }

    QString text;
    const auto append_section = [&text](const QString& title, const QStringList& paths) {
        if (paths.isEmpty()) {
            return;
        }
        if (!text.isEmpty()) {
            text += QLatin1Char('\n');
        }
        text += title + QStringLiteral(":\n");
        for (const auto& path : paths) {
            text += QStringLiteral("    %1\n").arg(path);
        }
    };

    append_section(QObject::tr("Modified"), modified);
    append_section(QObject::tr("Added"), added);
    append_section(QObject::tr("Unchanged"), unchanged);
    return text.trimmed();
}

} // namespace

bool confirmPatchApplication(
    QWidget& parent,
    const QString& target_root,
    const archive::PatchInspection& inspection) {
    int modified_count = 0;
    int added_count = 0;
    int unchanged_count = 0;
    for (const auto& file : inspection.files) {
        switch (file.state) {
        case archive::PatchFileState::Modified:
            ++modified_count;
            break;
        case archive::PatchFileState::Added:
            ++added_count;
            break;
        case archive::PatchFileState::Unchanged:
            ++unchanged_count;
            break;
        }
    }

    QDialog dialog(&parent);
    dialog.setWindowTitle(QObject::tr("Apply Downloaded ZIP"));
    dialog.resize(700, 500);

    auto* layout = new QVBoxLayout(&dialog);
    auto* target_label = new QLabel(
        QObject::tr("Target directory:\n%1").arg(target_root),
        &dialog);
    target_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    target_label->setWordWrap(true);
    layout->addWidget(target_label);

    auto* summary = new QLabel(
        QObject::tr("%1 modified, %2 added, %3 unchanged")
            .arg(modified_count)
            .arg(added_count)
            .arg(unchanged_count),
        &dialog);
    layout->addWidget(summary);

    auto* details = new QPlainTextEdit(&dialog);
    details->setReadOnly(true);
    details->setPlainText(reviewText(inspection));
    layout->addWidget(details, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        &dialog);
    if (auto* apply_button = buttons->button(QDialogButtonBox::Ok)) {
        apply_button->setText(QObject::tr("Apply Changes"));
        apply_button->setDefault(true);
    }
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    return dialog.exec() == QDialog::Accepted;
}

} // namespace ui
