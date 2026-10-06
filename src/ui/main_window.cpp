#include "ui/main_window.hpp"

#include "archive/patch_archive.hpp"
#include "archive/zip_archive.hpp"
#include "ui/directory_pane.hpp"
#include "ui/patch_review_dialog.hpp"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
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
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVariantMap>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <functional>
#include <memory>
#include <utility>

namespace ui {
namespace {

constexpr qint64 uploadChunkSize = 256 * 1024;
constexpr int attachmentVerificationIntervalMilliseconds = 250;
constexpr int attachmentVerificationAttempts = 16;

[[nodiscard]] QString compactJsonArray(const QJsonArray& values) {
    return QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QString fileInputExpression() {
    return QString::fromUtf8(R"JS(
(() => {
    const allInputs = [...document.querySelectorAll('input[type="file"]')];
    const isImageOnly = (input) => {
        const accept = (input.accept || '').trim().toLowerCase();
        if (!accept) return false;

        const tokens = accept.split(',').map((token) => token.trim()).filter(Boolean);
        return tokens.length > 0 && tokens.every((token) =>
            token.startsWith('image/') ||
            /^\.(?:avif|bmp|gif|heic|heif|jpe?g|png|svg|webp)$/.test(token));
    };
    const usable = (inputs) => inputs.filter((input) => !input.disabled && !isImageOnly(input));
    const describe = (input) => ({
        id: input.id || '',
        name: input.name || '',
        accept: input.accept || '',
        multiple: !!input.multiple,
        ariaLabel: input.getAttribute('aria-label') || '',
        testId: input.getAttribute('data-testid') || ''
    });

    const isVisible = (element) => element.getClientRects().length > 0;
    const prompt = [...document.querySelectorAll('#prompt-textarea')]
            .find(isVisible) ||
        [...document.querySelectorAll('[contenteditable="true"]')]
            .find(isVisible) ||
        [...document.querySelectorAll('textarea')]
            .find(isVisible) ||
        null;

    let candidates = [];
    if (prompt) {
        const form = prompt.closest('form');
        if (form) {
            candidates = usable([...form.querySelectorAll('input[type="file"]')]);
        }

        // The ChatGPT composer has not always used a <form>. Walk a few
        // ancestors so an upload input owned by the composer still wins over
        // unrelated file inputs elsewhere on the page.
        if (candidates.length === 0) {
            let ancestor = prompt.parentElement;
            for (let depth = 0; ancestor && depth < 7; ++depth, ancestor = ancestor.parentElement) {
                const nearby = usable([...ancestor.querySelectorAll('input[type="file"]')]);
                if (nearby.length > 0) {
                    candidates = nearby;
                    break;
                }
            }
        }
    }

    const choose = (inputs) => {
        if (inputs.length === 0) return null;
        if (inputs.length === 1) return inputs[0];

        const scored = inputs.map((input) => {
            const attributes = [
                input.id,
                input.name,
                input.getAttribute('aria-label'),
                input.getAttribute('data-testid')
            ].filter(Boolean).join(' ').toLowerCase();
            let score = 0;
            if (input.id === 'upload-files') score += 1000;
            if (/upload|attach|file/.test(attributes)) score += 100;
            if (input.multiple) score += 10;
            return { input, score };
        }).sort((left, right) => right.score - left.score);
        return scored[0].score > scored[1].score ? scored[0].input : null;
    };

    let input = choose(candidates);
    let method = input ? 'composer' : '';
    if (!input) {
        const exactUploads = usable(allInputs.filter((candidate) => candidate.id === 'upload-files'));
        input = exactUploads.length === 1 ? exactUploads[0] : null;
        if (input) method = 'upload-files-id';
    }
    if (!input) {
        const globalCandidates = usable(allInputs);
        input = globalCandidates.length === 1 ? globalCandidates[0] : null;
        if (input) method = 'only-file-input';
    }

    return {
        input,
        method,
        diagnostics: JSON.stringify({
            path: location.pathname,
            promptFound: !!prompt,
            fileInputs: allInputs.map(describe)
        })
    };
})()
)JS");
}

[[nodiscard]] QString dropTargetExpression() {
    return QString::fromUtf8(R"JS(
(() => {
    const isVisible = (element) => element.getClientRects().length > 0;
    const prompt = [...document.querySelectorAll('#prompt-textarea')]
            .find(isVisible) ||
        [...document.querySelectorAll('[contenteditable="true"]')]
            .find(isVisible) ||
        [...document.querySelectorAll('textarea')]
            .find(isVisible) ||
        null;
    return prompt;
})()
)JS");
}

// Chromium requires a genuine web user activation before it will open a file picker.
// Instead of synthesizing clicks, put the file directly into the upload input owned
// by the visible composer. If ChatGPT stops exposing that input, fall back to the
// composer's drag/drop path.
class FileInjector final : public QObject {
public:
    using Completion = std::function<void(bool)>;

    FileInjector(
        QWebEnginePage& page,
        QWidget& parent,
        QString file_path,
        Completion completion)
        : QObject(&parent),
          page_(&page),
          parent_(&parent),
          file_path_(std::move(file_path)),
          file_(file_path_),
          token_(QUuid::createUuid().toString(QUuid::WithoutBraces)),
          completion_(std::move(completion)) {
        javascript_timeout_.setSingleShot(true);
        connect(&javascript_timeout_, &QTimer::timeout, this, [this] {
            fail(tr("ChatZip timed out while %1.\n\nThe file is at:\n%2")
                     .arg(pending_stage_, file_path_));
        });
    }

    void start() {
        elapsed_.start();
        if (!file_.open(QIODevice::ReadOnly)) {
            fail(tr("ChatZip could not open the file for attachment:\n%1\n\n%2")
                     .arg(file_path_, file_.errorString()));
            return;
        }

        total_bytes_ = file_.size();
        total_chunks_ = total_bytes_ <= 0
                            ? 0
                            : (total_bytes_ + uploadChunkSize - 1) / uploadChunkSize;
        log(QStringLiteral("Attachment started: %1 (%2 bytes, %3 chunk(s))")
                .arg(file_path_)
                .arg(total_bytes_)
                .arg(total_chunks_));

        const auto file_name = QFileInfo(file_path_).fileName();
        const auto mime_type = QMimeDatabase().mimeTypeForFile(file_path_).name();
        const auto payload = compactJsonArray(QJsonArray{token_, file_name, mime_type});
        const auto input_expression = fileInputExpression();
        const auto script = QString::fromUtf8(R"JS(
(() => {
    const [token, fileName, mimeType] = __CHATZIP_PAYLOAD__;
    const lookup = __CHATZIP_INPUT__;

    window.__chatZipUploads ??= {};
    window.__chatZipUploads[token] = {
        fileName,
        mimeType,
        parts: []
    };
    return {
        ok: true,
        inputFound: !!lookup.input,
        inputMethod: lookup.method || '',
        diagnostics: lookup.diagnostics || ''
    };
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload)
                                .replace(QStringLiteral("__CHATZIP_INPUT__"), input_expression);

        beginJavascriptWait(tr("preparing the ChatGPT attachment"));
        QPointer<FileInjector> guarded(this);
        page_->runJavaScript(script, [guarded](const QVariant& value) {
            if (!guarded || guarded->terminal_) {
                return;
            }

            guarded->endJavascriptWait();
            const auto result = value.toMap();
            guarded->last_diagnostics_ = result.value(QStringLiteral("diagnostics")).toString();
            guarded->log(
                QStringLiteral("Composer upload input: %1 (%2)")
                    .arg(
                        result.value(QStringLiteral("inputFound")).toBool()
                            ? QStringLiteral("found")
                            : QStringLiteral("not found"),
                        result.value(QStringLiteral("inputMethod")).toString()));

            guarded->sendNextChunk();
        });
    }

private:
    static constexpr int javascriptTimeoutMilliseconds = 60 * 1000;

    void sendNextChunk() {
        if (terminal_) {
            return;
        }
        if (!page_) {
            fail(tr("The ChatGPT page closed while ChatZip was attaching the file.\n\n%1")
                     .arg(file_path_));
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

    const binary = atob(chunk);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; ++i) {
        bytes[i] = binary.charCodeAt(i);
    }
    upload.parts.push(bytes);
    return true;
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload);

        beginJavascriptWait(tr("sending attachment data to ChatGPT"));
        QPointer<FileInjector> guarded(this);
        page_->runJavaScript(script, [guarded](const QVariant& value) {
            if (!guarded || guarded->terminal_) {
                return;
            }

            guarded->endJavascriptWait();
            if (!value.toBool()) {
                guarded->fail(
                    tr("ChatGPT discarded the pending attachment before it was complete.\n\n"
                       "The file is at:\n%1")
                        .arg(guarded->file_path_));
                return;
            }

            ++guarded->chunks_sent_;
            if (guarded->chunks_sent_ == guarded->total_chunks_ ||
                guarded->chunks_sent_ % 10 == 0) {
                guarded->log(
                    QStringLiteral("Attachment chunks sent: %1/%2")
                        .arg(guarded->chunks_sent_)
                        .arg(guarded->total_chunks_));
            }
            guarded->sendNextChunk();
        });
    }

    void finalizeUpload() {
        if (!page_) {
            fail(tr("The ChatGPT page closed while ChatZip was finalizing the attachment.\n\n%1")
                     .arg(file_path_));
            return;
        }

        const auto payload = compactJsonArray(QJsonArray{token_});
        const auto input_expression = fileInputExpression();
        const auto drop_target_expression = dropTargetExpression();
        const auto script = QString::fromUtf8(R"JS(
(() => {
    const [token] = __CHATZIP_PAYLOAD__;
    const upload = window.__chatZipUploads?.[token];
    if (!upload) {
        return { ok: false, reason: 'Pending upload data was lost.' };
    }

    try {
        const file = new File(upload.parts, upload.fileName, {
            type: upload.mimeType || 'application/octet-stream',
            lastModified: Date.now()
        });
        const lookup = __CHATZIP_INPUT__;
        const input = lookup.input;
        if (input) {
            const transfer = new DataTransfer();
            transfer.items.add(file);
            input.files = transfer.files;

            input.dispatchEvent(new Event('input', { bubbles: true, composed: true }));
            input.dispatchEvent(new Event('change', { bubbles: true, composed: true }));

            return {
                ok: true,
                method: 'input',
                fileName: file.name,
                fileSize: file.size,
                inputId: input.id || '',
                inputMethod: lookup.method || '',
                diagnostics: lookup.diagnostics || ''
            };
        }

        const dropTarget = __CHATZIP_DROP_TARGET__;
        if (!dropTarget) {
            return {
                ok: false,
                reason: 'Neither a ChatGPT upload input nor the message composer was found.',
                diagnostics: lookup.diagnostics || ''
            };
        }

        const transfer = new DataTransfer();
        transfer.items.add(file);
        for (const eventName of ['dragenter', 'dragover', 'drop']) {
            dropTarget.dispatchEvent(new DragEvent(eventName, {
                bubbles: true,
                cancelable: true,
                composed: true,
                dataTransfer: transfer
            }));
        }

        return {
            ok: true,
            method: 'drop',
            fileName: file.name,
            fileSize: file.size,
            diagnostics: lookup.diagnostics || ''
        };
    } catch (error) {
        return {
            ok: false,
            reason: error instanceof Error ? error.message : String(error)
        };
    }
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload)
                                .replace(QStringLiteral("__CHATZIP_INPUT__"), input_expression)
                                .replace(
                                    QStringLiteral("__CHATZIP_DROP_TARGET__"),
                                    drop_target_expression);

        beginJavascriptWait(tr("handing the completed file to ChatGPT"));
        QPointer<FileInjector> guarded(this);
        page_->runJavaScript(script, [guarded](const QVariant& value) {
            if (!guarded || guarded->terminal_) {
                return;
            }

            guarded->endJavascriptWait();
            const auto result = value.toMap();
            if (!result.value(QStringLiteral("ok")).toBool()) {
                guarded->fail(
                    tr("ChatZip could not hand the file to ChatGPT's composer.\n\n%1\n\n"
                       "The file is at:\n%2")
                        .arg(
                            result.value(QStringLiteral("reason")).toString(),
                            guarded->file_path_));
                return;
            }

            guarded->last_dispatch_method_ = result.value(QStringLiteral("method")).toString();
            guarded->last_diagnostics_ = result.value(QStringLiteral("diagnostics")).toString();
            guarded->log(
                QStringLiteral("Attachment dispatched through %1")
                    .arg(guarded->last_dispatch_method_));
            guarded->verifyUpload(attachmentVerificationAttempts, true);
        });
    }

    void verifyUpload(int attempts_remaining, bool allow_drop_fallback) {
        if (terminal_) {
            return;
        }
        if (!page_) {
            fail(tr("The ChatGPT page closed while ChatZip was verifying the attachment.\n\n%1")
                     .arg(file_path_));
            return;
        }

        const auto payload = compactJsonArray(QJsonArray{token_});
        const auto script = QString::fromUtf8(R"JS(
(() => {
    const [token] = __CHATZIP_PAYLOAD__;
    const upload = window.__chatZipUploads?.[token];
    if (!upload) {
        return { ok: false, lost: true };
    }

    const fileName = upload.fileName;
    const bodyText = document.body?.innerText || '';
    if (bodyText.includes(fileName)) {
        return { ok: true, evidence: 'text' };
    }

    const attributed = [...document.querySelectorAll('[aria-label], [title]')]
        .some((element) =>
            (element.getAttribute('aria-label') || '').includes(fileName) ||
            (element.getAttribute('title') || '').includes(fileName));
    return { ok: attributed, evidence: attributed ? 'attribute' : '' };
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload);

        beginJavascriptWait(tr("verifying that ChatGPT accepted the attachment"));
        QPointer<FileInjector> guarded(this);
        page_->runJavaScript(
            script,
            [guarded, attempts_remaining, allow_drop_fallback](const QVariant& value) {
                if (!guarded || guarded->terminal_) {
                    return;
                }

                guarded->endJavascriptWait();
                const auto result = value.toMap();
                if (result.value(QStringLiteral("ok")).toBool()) {
                    guarded->log(
                        QStringLiteral("Attachment visible in ChatGPT (%1)")
                            .arg(result.value(QStringLiteral("evidence")).toString()));
                    guarded->succeed();
                    return;
                }
                if (result.value(QStringLiteral("lost")).toBool()) {
                    guarded->fail(
                        tr("ChatGPT discarded the pending attachment while ChatZip was verifying it.\n\n"
                           "The file is at:\n%1")
                            .arg(guarded->file_path_));
                    return;
                }

                if (attempts_remaining > 1) {
                    QTimer::singleShot(
                        attachmentVerificationIntervalMilliseconds,
                        guarded,
                        [guarded, attempts_remaining, allow_drop_fallback] {
                            if (guarded) {
                                guarded->verifyUpload(
                                    attempts_remaining - 1,
                                    allow_drop_fallback);
                            }
                        });
                    return;
                }

                if (allow_drop_fallback && guarded->last_dispatch_method_ == QStringLiteral("input")) {
                    guarded->log(
                        QStringLiteral("No attachment appeared after input events; trying composer drop fallback"));
                    guarded->dispatchDropFallback();
                    return;
                }

                auto detail = tr("ChatZip sent the file to the page, but ChatGPT did not show an attachment.\n\n"
                                 "This usually means ChatGPT changed its composer DOM or upload handling.\n\n"
                                 "The file is at:\n%1")
                                  .arg(guarded->file_path_);
                if (!guarded->last_diagnostics_.isEmpty()) {
                    detail += tr("\n\nPage diagnostics:\n%1").arg(guarded->last_diagnostics_);
                }
                guarded->fail(detail);
            });
    }

    void dispatchDropFallback() {
        if (terminal_) {
            return;
        }
        if (!page_) {
            fail(tr("The ChatGPT page closed while ChatZip was retrying the attachment.\n\n%1")
                     .arg(file_path_));
            return;
        }

        const auto payload = compactJsonArray(QJsonArray{token_});
        const auto drop_target_expression = dropTargetExpression();
        const auto script = QString::fromUtf8(R"JS(
(() => {
    const [token] = __CHATZIP_PAYLOAD__;
    const upload = window.__chatZipUploads?.[token];
    if (!upload) {
        return { ok: false, reason: 'Pending upload data was lost.' };
    }

    try {
        const dropTarget = __CHATZIP_DROP_TARGET__;
        if (!dropTarget) {
            return { ok: false, reason: 'The ChatGPT message composer was not found.' };
        }

        const file = new File(upload.parts, upload.fileName, {
            type: upload.mimeType || 'application/octet-stream',
            lastModified: Date.now()
        });
        const transfer = new DataTransfer();
        transfer.items.add(file);
        for (const eventName of ['dragenter', 'dragover', 'drop']) {
            dropTarget.dispatchEvent(new DragEvent(eventName, {
                bubbles: true,
                cancelable: true,
                composed: true,
                dataTransfer: transfer
            }));
        }
        return { ok: true };
    } catch (error) {
        return {
            ok: false,
            reason: error instanceof Error ? error.message : String(error)
        };
    }
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload)
                                .replace(
                                    QStringLiteral("__CHATZIP_DROP_TARGET__"),
                                    drop_target_expression);

        beginJavascriptWait(tr("retrying the attachment through the ChatGPT composer"));
        QPointer<FileInjector> guarded(this);
        page_->runJavaScript(script, [guarded](const QVariant& value) {
            if (!guarded || guarded->terminal_) {
                return;
            }

            guarded->endJavascriptWait();
            const auto result = value.toMap();
            if (!result.value(QStringLiteral("ok")).toBool()) {
                guarded->fail(
                    tr("ChatZip could not retry the attachment through the ChatGPT composer.\n\n%1\n\n"
                       "The file is at:\n%2")
                        .arg(
                            result.value(QStringLiteral("reason")).toString(),
                            guarded->file_path_));
                return;
            }

            guarded->last_dispatch_method_ = QStringLiteral("drop");
            guarded->verifyUpload(attachmentVerificationAttempts, false);
        });
    }

    void beginJavascriptWait(const QString& stage) {
        pending_stage_ = stage;
        javascript_timeout_.start(javascriptTimeoutMilliseconds);
    }

    void endJavascriptWait() {
        javascript_timeout_.stop();
        pending_stage_.clear();
    }

    void cleanupPendingUpload() {
        if (!page_) {
            return;
        }

        const auto payload = compactJsonArray(QJsonArray{token_});
        const auto script = QString::fromUtf8(R"JS(
(() => {
    const [token] = __CHATZIP_PAYLOAD__;
    if (window.__chatZipUploads) {
        delete window.__chatZipUploads[token];
    }
})()
)JS")
                                .replace(QStringLiteral("__CHATZIP_PAYLOAD__"), payload);
        page_->runJavaScript(script);
    }

    void succeed() {
        if (terminal_) {
            return;
        }

        terminal_ = true;
        javascript_timeout_.stop();
        cleanupPendingUpload();
        log(QStringLiteral("Attachment accepted by browser in %1 ms").arg(elapsed_.elapsed()));
        auto completion = std::move(completion_);
        if (completion) {
            completion(true);
        }
        deleteLater();
    }

    void fail(const QString& detail) {
        if (terminal_) {
            return;
        }

        terminal_ = true;
        javascript_timeout_.stop();
        cleanupPendingUpload();
        log(QStringLiteral("Attachment failed after %1 ms: %2")
                .arg(elapsed_.isValid() ? elapsed_.elapsed() : 0)
                .arg(detail));
        if (parent_) {
            QMessageBox::warning(parent_, tr("Could Not Attach File"), detail);
        }
        auto completion = std::move(completion_);
        if (completion) {
            completion(false);
        }
        deleteLater();
    }

    static void log(const QString& message) {
        qInfo().noquote()
            << QStringLiteral("%1 [ChatZip] %2")
                   .arg(
                       QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                       message);
    }

    QPointer<QWebEnginePage> page_;
    QPointer<QWidget> parent_;
    QString file_path_;
    QFile file_;
    QString token_;
    Completion completion_;
    QTimer javascript_timeout_;
    QElapsedTimer elapsed_;
    QString pending_stage_;
    qint64 total_bytes_{};
    qint64 total_chunks_{};
    qint64 chunks_sent_{};
    QString last_dispatch_method_;
    QString last_diagnostics_;
    bool terminal_{};
};

void attachFile(
    QWebEnginePage& page,
    QWidget& parent,
    const QString& file_path,
    FileInjector::Completion completion) {
    auto* injector = new FileInjector(page, parent, file_path, std::move(completion));
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
        [this, directory_pane, web_page](const QString& file_path) {
            if (operation_in_progress_) {
                QMessageBox::information(
                    this,
                    tr("Attachment In Progress"),
                    tr("ChatZip is already creating or attaching a file."));
                return;
            }

            operation_in_progress_ = true;
            directory_pane->setZipAttachBusy(true, tr("Attaching..."));
            QPointer<MainWindow> guarded_window(this);
            QPointer<DirectoryPane> guarded_pane(directory_pane);
            attachFile(
                *web_page,
                *this,
                file_path,
                [guarded_window, guarded_pane](bool) {
                    if (!guarded_window) {
                        return;
                    }
                    guarded_window->operation_in_progress_ = false;
                    if (guarded_pane) {
                        guarded_pane->setZipAttachBusy(false);
                    }
                });
        });

    connect(
        directory_pane,
        &DirectoryPane::zipAttachRequested,
        this,
        [this, directory_pane, web_page] {
            if (operation_in_progress_) {
                return;
            }

            static QTemporaryDir temporary_directory(
                QDir::tempPath() + QStringLiteral("/ChatZip-XXXXXX"));
            if (!temporary_directory.isValid()) {
                QMessageBox::warning(
                    this,
                    tr("Could Not Create ZIP"),
                    tr("ChatZip could not create its temporary directory."));
                return;
            }

            const auto root_directory = directory_pane->rootDirectory();
            const auto selected_paths = directory_pane->selectedPaths();
            const auto archive_path = makeArchivePath(temporary_directory, root_directory);

            operation_in_progress_ = true;
            directory_pane->setZipAttachBusy(true, tr("Creating ZIP..."));
            qInfo().noquote()
                << QStringLiteral("%1 [ChatZip] ZIP requested: %2 selection(s) from %3")
                       .arg(
                           QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                       .arg(selected_paths.size())
                       .arg(root_directory);

            auto result = std::make_shared<archive::ZipResult>();
            auto* worker = QThread::create(
                [result, root_directory, selected_paths, archive_path] {
                    *result = archive::createZipArchive(
                        root_directory, selected_paths, archive_path);
                });
            connect(
                worker,
                &QThread::finished,
                this,
                [this, directory_pane, web_page, archive_path, result] {
                    if (!result->success) {
                        operation_in_progress_ = false;
                        directory_pane->setZipAttachBusy(false);
                        QMessageBox::warning(
                            this,
                            tr("Could Not Create ZIP"),
                            result->error);
                        return;
                    }

                    directory_pane->setZipAttachBusy(true, tr("Attaching..."));
                    QPointer<MainWindow> guarded_window(this);
                    QPointer<DirectoryPane> guarded_pane(directory_pane);
                    attachFile(
                        *web_page,
                        *this,
                        archive_path,
                        [guarded_window, guarded_pane](bool) {
                            if (!guarded_window) {
                                return;
                            }
                            guarded_window->operation_in_progress_ = false;
                            if (guarded_pane) {
                                guarded_pane->setZipAttachBusy(false);
                            }
                        });
                });
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            worker->start();
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
