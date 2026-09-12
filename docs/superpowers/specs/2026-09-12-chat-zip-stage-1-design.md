# ChatZip Stage 1 Design

## Goal

Create the first working version of ChatZip, a C++23 / Qt 6 desktop application that embeds ChatGPT and provides a filesystem tree for choosing the source directory that later ZIP-transfer features will operate on.

## Scope

Stage 1 intentionally does not create ZIP archives, attach files to ChatGPT, intercept downloads, or apply returned files. Those behaviors belong to later commits.

## Architecture

The application is a single Qt Widgets executable named `chat_zip`.

`main.cpp` owns a named `QWebEngineProfile` for the lifetime of the application window so browser cookies and other persistent browser data can survive restarts. `ui::MainWindow` receives that profile, owns the top-level horizontal splitter, and places a `QWebEngineView` loading `https://chatgpt.com/` on the left side.

`ui::DirectoryPane` owns the right-side project browser. It contains an `Open Directory...` button, a label showing the current root, a `QFileSystemModel`, and a `QTreeView`. The tree starts disabled until the user chooses a directory. Choosing a directory calls `setRootDirectory()`, sets the filesystem model root, points the tree at the resulting model index, updates the label, and enables the tree.

`DirectoryPane` is kept separate from `MainWindow` because Stage 2 will add selection/packaging behavior there without coupling filesystem logic to the browser implementation.

## UI

The initial window is approximately 1400x900. The web view receives most of the horizontal space, while the directory pane starts around 360 pixels wide. The directory tree shows the standard filesystem Name, Size, Type, and Date Modified columns and supports extended selection in preparation for later packaging work.

## Build

Use a top-level `CMakeLists.txt` modeled after the supplied `stick_man` project: CMake 3.16 minimum, C++23, Qt automoc/uic/rcc enabled, an explicit source list in `add_executable`, and explicit `target_link_libraries` entries. The application requires Qt 6 Widgets and WebEngineWidgets.

When `BUILD_TESTING` is enabled, build a Qt Test executable for `DirectoryPane`.

## Tests

The `DirectoryPane` test creates a temporary directory, assigns it as the root, then verifies that:

- the pane reports the selected directory;
- the tree becomes enabled;
- the tree uses a `QFileSystemModel`;
- the tree root index resolves to the chosen directory.

The embedded ChatGPT page is not network-tested; its startup URL is deterministic application wiring and loading the live service would make the unit test depend on external networking/authentication.
