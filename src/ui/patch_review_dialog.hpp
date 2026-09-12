#pragma once

#include <QString>

class QWidget;

namespace archive {
struct PatchInspection;
}

namespace ui {

[[nodiscard]] bool confirmPatchApplication(
    QWidget& parent,
    const QString& target_root,
    const archive::PatchInspection& inspection);

} // namespace ui
