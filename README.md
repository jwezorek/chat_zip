# ChatZip

ChatZip is a small C++23 / Qt 6 desktop application for a ZIP-based coding workflow with ChatGPT. It keeps an ordinary ChatGPT web session in an embedded browser while providing a local project tree beside it, so source files can be sent to ChatGPT and returned changes can be reviewed and applied without repeatedly creating, locating, and unpacking ZIP files by hand.

The intended workflow is:

1. Open the local project directory in the filesystem pane.
2. Select one or more files or directories in the tree.
3. Click **Zip & Attach** to package the selection and attach the resulting ZIP to the current ChatGPT conversation.
4. Ask ChatGPT to make a change and return a ZIP containing the changed or added files, with paths relative to the project root.
5. Download that ZIP from the embedded ChatGPT page.
6. Review which files are modified, added, or unchanged and choose whether to apply the changes to the local project.

ChatZip is deliberately not an autonomous coding agent or a replacement for source control. ChatGPT remains an ordinary interactive web session, and Git remains the expected recovery/history mechanism. ChatZip only automates the file-transfer and patch-application parts of the workflow.

## Current functionality

### Embedded ChatGPT browser

- Opens `https://chatgpt.com/` in a `QWebEngineView`.
- Uses a named, persistent `QWebEngineProfile` with persistent cookies so the ChatGPT login/session can survive application restarts.
- Leaves normal interaction with ChatGPT in the web page itself; ChatZip only hooks the upload and download paths needed for the ZIP workflow.

### Local project browser

- Displays a filesystem tree using `QFileSystemModel` and `QTreeView`.
- **Open Directory...** chooses the project root shown in the tree.
- Supports extended selection, so multiple files and/or directories can be selected at once.
- The selected root directory is also the target directory used when a returned ZIP is inspected and applied.

### Zip & Attach

- **Zip & Attach** is enabled when a project root is open and at least one tree item is selected.
- Selected files are added to the archive using paths relative to the project root.
- Selected directories are traversed recursively and all regular files beneath them are included.
- Duplicate archive entries are suppressed when selections overlap.
- Items outside the selected project root are rejected.
- ZIP creation uses the bundled `miniz` implementation and writes the archive to a temporary ChatZip directory.
- The prepared ZIP is attached to ChatGPT by placing it into ChatGPT's file input from the embedded WebEngine page and dispatching the corresponding input/change events. The archive is transferred into the page in chunks rather than encoded as one large JavaScript string.

Because the attachment integration uses ChatGPT's web UI, a future ChatGPT DOM change may require the file-input detection code to be updated.

### Downloaded ZIP review and application

ZIP downloads initiated in the embedded browser are intercepted by ChatZip. A ZIP is first downloaded to a temporary directory rather than directly over the local project.

Before offering to apply it, ChatZip inspects the archive and rejects unsafe or incompatible contents, including:

- absolute paths, `.` / `..` path traversal, empty path components, drive-qualified paths, and Windows-invalid path components;
- duplicate or case-colliding archive paths;
- file/directory path conflicts;
- symbolic-link or junction targets, or paths that would pass through one;
- unsupported/encrypted archive entries;
- attempts to replace a directory with a file or otherwise target a non-regular file;
- the common mistaken layout where the ZIP contains an extra top-level project directory even though the selected target is already that project directory.

For each regular file in a valid ZIP, ChatZip compares the downloaded contents with the corresponding file under the selected project root and classifies it as:

- **Modified** — the target file already exists but has different contents;
- **Added** — the target file does not yet exist;
- **Unchanged** — the target file already has identical contents.

If every file is unchanged, nothing is applied. Otherwise ChatZip displays a review dialog listing the files in each category and requires explicit confirmation before making changes.

When changes are applied:

- missing parent directories are created as needed;
- unchanged files are skipped;
- existing file permissions are preserved when replacing a file;
- writes use `QSaveFile` so replacement is committed atomically where Qt supports it;
- the target files are revalidated against the state that was shown in the review dialog, preventing a local edit made while the dialog is open from being silently overwritten.

Returned ZIPs currently add or replace files only. They do not delete files from the target project.

Non-ZIP browser downloads are not treated as patches; ChatZip presents a normal save-file dialog for them.

## Requirements

- CMake 3.16 or newer
- C++23-capable compiler
- Qt 6 with the Widgets and WebEngineWidgets modules
- Qt Test when building tests

Windows is the primary platform.

## Build

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\msvc2022_64
cmake --build build --config Debug
```

If your Qt installation is already discoverable by CMake, omit `CMAKE_PREFIX_PATH`.

Run tests with a single-config generator using:

```powershell
ctest --test-dir build --output-on-failure
```

For Visual Studio generators, add `-C Debug` to the `ctest` command.
