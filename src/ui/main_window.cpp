#include "ui/main_window.hpp"

#include "archive/patch_archive.hpp"
#include "archive/zip_archive.hpp"
#include "ui/directory_pane.hpp"
#include "ui/patch_review_dialog.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QSplitter>
#include <QTemporaryDir>
#include <QUuid>
#include <QUrl>
#include <QVariantMap>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <utility>

namespace ui {
namespace {

constexpr qint64 uploadChunkSize = 256 * 1024;

[[nodiscard]] QString compactJsonArray(const QJsonArray& values) {
    return QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString fileInputExpression() {
    return QStringLiteral(
        "document.querySelector('input#upload-files') || "
        "[...document.querySelectorAll('input[type=\\\"file\\\"]')]"
        ".find((input) => !/photo|camera/i.test(input.id || '')) || "
        "document.querySelector('input[type=\\\"file\\\"]')");
}

// Chromium requires a genuine web user activation before it will open a file picker.
// Instead of synthesizing clicks, put the file directly into ChatGPT's hidden file
// input and dispatch the same input/change events a picker would produce.
class FileInjector final : public QObject {
public:
    FileInjector(
        QWebEnginePage& page,
        QWidget& parent,
        QString file_path)
        : QObject(&page),
          page_(&page),
          parent_(&parent),
          file_path_(std::move(file_path)),
          file_(file_path_),
          token_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

    void start() {
        if (!file_.open(QIODevice::ReadOnly)) {
            fail(tr("ChatZip could not open the file for attachment:\n%1\n\n%2")
                     .arg(file_path_, file_.errorString()));
            return;
        }

        const auto file_name = QFileInfo(file_path_).fileName();
        const auto mime_type = QMimeDatabase().mimeTypeForFile(file_path_).name();
        const auto payload = compactJsonArray(QJsonArray{token_, file_name, mime_type});
        const auto input_expression = fileInputExpression();
        const auto script = QString::fromUtf8(R"JS(
(() => {
    const [token, fileName, mimeType] = __CHATZIP_PAYLOAD__;
    const input = __CHATZIP_INPUT__;
    if (!input) {
        return { ok: false, reason: 'ChatGPT file input was not found.' };
    }

    window.__chatZipUploads ??= {};
    window.__chatZipUploads[token] = {
        fileName,
        mimeType,
        chunks: []
    };
    return { ok: true, inputId: input.id || '' };
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload)
                                .replace(QStringLiteral("__CHATZIP_INPUT__"), input_expression);

        QPointer<FileInjector> guarded(this);
        page_->runJavaScript(script, [guarded](const QVariant& value) {
            if (!guarded) {
                return;
            }

            const auto result = value.toMap();
            if (!result.value(QStringLiteral("ok")).toBool()) {
                guarded->fail(
                    tr("ChatZip could not find ChatGPT's file input.\n\n%1\n\n"
                       "The file is at:\n%2")
                        .arg(
                            result.value(QStringLiteral("reason")).toString(),
                            guarded->file_path_));
                return;
            }

            guarded->sendNextChunk();
        });
    }

private:
    void sendNextChunk() {
        if (!page_) {
            deleteLater();
            return;
        }

        const auto chunk = file_.read(uploadChunkSize);
        if (chunk.isEmpty()) {
            if (file_.error() != QFile::NoError) {
                fail(tr("ChatZip could not read the file while attaching it:\n%1\n\n%2")
                         .arg(file_path_, file_.errorString()));
                return;
            }

            finalizeUpload();
            return;
        }

        const auto encoded = QString::fromLatin1(chunk.toBase64());
        const auto payload = compactJsonArray(QJsonArray{token_, encoded});
        const auto script = QString::fromUtf8(R"JS(
(() => {
    const [token, chunk] = __CHATZIP_PAYLOAD__;
    const upload = window.__chatZipUploads?.[token];
    if (!upload) return false;
    upload.chunks.push(chunk);
    return true;
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload);

        QPointer<FileInjector> guarded(this);
        page_->runJavaScript(script, [guarded](const QVariant& value) {
            if (!guarded) {
                return;
            }

            if (!value.toBool()) {
                guarded->fail(
                    tr("ChatGPT discarded the pending attachment before it was complete.\n\n"
                       "The file is at:\n%1")
                        .arg(guarded->file_path_));
                return;
            }

            guarded->sendNextChunk();
        });
    }

    void finalizeUpload() {
        const auto payload = compactJsonArray(QJsonArray{token_});
        const auto input_expression = fileInputExpression();
        const auto script = QString::fromUtf8(R"JS(
(() => {
    const [token] = __CHATZIP_PAYLOAD__;
    const upload = window.__chatZipUploads?.[token];
    if (!upload) {
        return { ok: false, reason: 'Pending upload data was lost.' };
    }

    try {
        const input = __CHATZIP_INPUT__;
        if (!input) {
            return { ok: false, reason: 'ChatGPT file input disappeared.' };
        }

        const parts = upload.chunks.map((chunk) => {
            const binary = atob(chunk);
            const bytes = new Uint8Array(binary.length);
            for (let i = 0; i < binary.length; ++i) {
                bytes[i] = binary.charCodeAt(i);
            }
            return bytes;
        });

        const file = new File(parts, upload.fileName, {
            type: upload.mimeType || 'application/octet-stream',
            lastModified: Date.now()
        });
        const transfer = new DataTransfer();
        transfer.items.add(file);
        input.files = transfer.files;

        input.dispatchEvent(new Event('input', { bubbles: true, composed: true }));
        input.dispatchEvent(new Event('change', { bubbles: true, composed: true }));

        return {
            ok: true,
            fileName: file.name,
            fileSize: file.size,
            inputId: input.id || ''
        };
    } catch (error) {
        return {
            ok: false,
            reason: error instanceof Error ? error.message : String(error)
        };
    } finally {
        delete window.__chatZipUploads[token];
    }
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload)
                                .replace(QStringLiteral("__CHATZIP_INPUT__"), input_expression);

        QPointer<FileInjector> guarded(this);
        page_->runJavaScript(script, [guarded](const QVariant& value) {
            if (!guarded) {
                return;
            }

            const auto result = value.toMap();
            if (!result.value(QStringLiteral("ok")).toBool()) {
                guarded->fail(
                    tr("ChatZip could not place the file into ChatGPT's upload input.\n\n%1\n\n"
                       "The file is at:\n%2")
                        .arg(
                            result.value(QStringLiteral("reason")).toString(),
                            guarded->file_path_));
                return;
            }

            guarded->deleteLater();
        });
    }

    void fail(const QString& detail) {
        if (parent_) {
            QMessageBox::warning(
                parent_,
                tr("Could Not Attach File"),
                detail);
        }
        deleteLater();
    }

    QPointer<QWebEnginePage> page_;
    QPointer<QWidget> parent_;
    QString file_path_;
    QFile file_;
    QString token_;
};

void attachFile(
    QWebEnginePage& page,
    QWidget& parent,
    const QString& file_path) {
    auto* injector = new FileInjector(page, parent, file_path);
    injector->start();
}

[[nodiscard]] QString makeArchivePath(QTemporaryDir& temporary_directory, const QString& root_directory) {
    auto project_name = QFileInfo(root_directory).fileName();
    if (project_name.isEmpty()) {
        project_name = QStringLiteral("selection");
    }
    const auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    return temporary_directory.filePath(
        QStringLiteral("%1-selection-%2.zip").arg(project_name, timestamp));
}

[[nodiscard]] bool isZipDownload(const QWebEngineDownloadRequest& download) {
    const auto file_name = download.suggestedFileName().isEmpty()
                               ? download.downloadFileName()
                               : download.suggestedFileName();
    return QFileInfo(file_name).suffix().compare(QStringLiteral("zip"), Qt::CaseInsensitive) == 0 ||
           download.mimeType().contains(QStringLiteral("zip"), Qt::CaseInsensitive);
}

[[nodiscard]] int changedFileCount(const archive::PatchInspection& inspection) {
    int count = 0;
    for (const auto& file : inspection.files) {
        if (file.state != archive::PatchFileState::Unchanged) {
            ++count;
        }
    }
    return count;
}

void removeDownloadDirectory(const QString& path) {
    QDir directory(path);
    if (directory.exists()) {
        directory.removeRecursively();
    }
}

} // namespace

MainWindow::MainWindow(QWebEngineProfile& web_profile, QWidget* parent)
    : QMainWindow(parent) {
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* web_view = new QWebEngineView(splitter);
    auto* directory_pane = new DirectoryPane(splitter);

    auto* web_page = new QWebEnginePage(&web_profile, web_view);
    web_view->setPage(web_page);

    connect(
        &web_profile,
        &QWebEngineProfile::downloadRequested,
        this,
        [this, directory_pane](QWebEngineDownloadRequest* download) {
            if (!download) {
                return;
            }

            if (!isZipDownload(*download)) {
                const auto file_name = download->downloadFileName().isEmpty()
                                           ? download->suggestedFileName()
                                           : download->downloadFileName();
                const auto initial_path = QDir(download->downloadDirectory()).filePath(file_name);
                const auto save_path = QFileDialog::getSaveFileName(
                    this,
                    tr("Save Download"),
                    initial_path);
                if (save_path.isEmpty()) {
                    download->cancel();
                    return;
                }

                const QFileInfo save_info(save_path);
                download->setDownloadDirectory(save_info.absolutePath());
                download->setDownloadFileName(save_info.fileName());
                connect(
                    download,
                    &QWebEngineDownloadRequest::isFinishedChanged,
                    download,
                    [download] {
                        if (download->isFinished()) {
                            download->deleteLater();
                        }
                    });
                download->accept();
                return;
            }

            const auto target_root = directory_pane->rootDirectory();
            if (target_root.isEmpty()) {
                QMessageBox::warning(
                    this,
                    tr("No Target Directory"),
                    tr("Select the project directory in ChatZip before downloading a ZIP patch."));
                download->cancel();
                return;
            }

            static QTemporaryDir download_temporary_directory(
                QDir::tempPath() + QStringLiteral("/ChatZip-downloads-XXXXXX"));
            if (!download_temporary_directory.isValid()) {
                QMessageBox::warning(
                    this,
                    tr("Could Not Download ZIP"),
                    tr("ChatZip could not create its temporary download directory."));
                download->cancel();
                return;
            }

            const auto download_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto download_directory = download_temporary_directory.filePath(download_id);
            if (!QDir().mkpath(download_directory)) {
                QMessageBox::warning(
                    this,
                    tr("Could Not Download ZIP"),
                    tr("ChatZip could not create a temporary directory for the ZIP download."));
                download->cancel();
                return;
            }

            const auto archive_path = QDir(download_directory).filePath(QStringLiteral("response.zip"));
            download->setDownloadDirectory(download_directory);
            download->setDownloadFileName(QStringLiteral("response.zip"));

            QPointer<QWebEngineDownloadRequest> guarded(download);
            connect(
                download,
                &QWebEngineDownloadRequest::stateChanged,
                this,
                [this, guarded, archive_path, download_directory, target_root](
                    QWebEngineDownloadRequest::DownloadState state) {
                    if (!guarded) {
                        removeDownloadDirectory(download_directory);
                        return;
                    }

                    if (state == QWebEngineDownloadRequest::DownloadInterrupted) {
                        QMessageBox::warning(
                            this,
                            tr("ZIP Download Failed"),
                            guarded->interruptReasonString().isEmpty()
                                ? tr("The ZIP download was interrupted.")
                                : guarded->interruptReasonString());
                        removeDownloadDirectory(download_directory);
                        guarded->deleteLater();
                        return;
                    }
                    if (state == QWebEngineDownloadRequest::DownloadCancelled) {
                        removeDownloadDirectory(download_directory);
                        guarded->deleteLater();
                        return;
                    }
                    if (state != QWebEngineDownloadRequest::DownloadCompleted) {
                        return;
                    }

                    const auto inspection = archive::inspectPatchArchive(archive_path, target_root);
                    if (!inspection.success) {
                        QMessageBox::warning(
                            this,
                            tr("ZIP Does Not Match Target"),
                            inspection.error);
                        removeDownloadDirectory(download_directory);
                        guarded->deleteLater();
                        return;
                    }

                    const auto changed_count = changedFileCount(inspection);
                    if (changed_count == 0) {
                        QMessageBox::information(
                            this,
                            tr("No Changes"),
                            tr("Every file in the downloaded ZIP already matches the target directory."));
                        removeDownloadDirectory(download_directory);
                        guarded->deleteLater();
                        return;
                    }

                    if (confirmPatchApplication(*this, target_root, inspection)) {
                        const auto result = archive::applyPatch(target_root, inspection);
                        if (!result.success) {
                            QMessageBox::warning(this, tr("Could Not Apply ZIP"), result.error);
                        } else {
                            QMessageBox::information(
                                this,
                                tr("Changes Applied"),
                                tr("Applied %1 changed file(s) to:\n%2")
                                    .arg(changed_count)
                                    .arg(target_root));
                        }
                    }

                    removeDownloadDirectory(download_directory);
                    guarded->deleteLater();
                });

            download->accept();
        });

    web_view->load(QUrl(QStringLiteral("https://chatgpt.com/")));

    connect(
        directory_pane,
        &DirectoryPane::fileAttachRequested,
        this,
        [this, web_page](const QString& file_path) {
            attachFile(*web_page, *this, file_path);
        });

    connect(
        directory_pane,
        &DirectoryPane::zipAttachRequested,
        this,
        [this, directory_pane, web_page] {
            static QTemporaryDir temporary_directory(
                QDir::tempPath() + QStringLiteral("/ChatZip-XXXXXX"));
            if (!temporary_directory.isValid()) {
                QMessageBox::warning(
                    this,
                    tr("Could Not Create ZIP"),
                    tr("ChatZip could not create its temporary directory."));
                return;
            }

            const auto archive_path =
                makeArchivePath(temporary_directory, directory_pane->rootDirectory());
            const auto result = archive::createZipArchive(
                directory_pane->rootDirectory(), directory_pane->selectedPaths(), archive_path);
            if (!result.success) {
                QMessageBox::warning(this, tr("Could Not Create ZIP"), result.error);
                return;
            }

            attachFile(*web_page, *this, archive_path);
        });

    directory_pane->setMinimumWidth(300);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes(QList<int>{1040, 360});

    setCentralWidget(splitter);
    setWindowTitle(tr("ChatZip"));
    resize(1400, 900);
}

} // namespace ui
