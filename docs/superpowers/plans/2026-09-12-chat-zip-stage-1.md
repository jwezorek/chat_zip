# ChatZip Stage 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a C++23 / Qt 6 desktop application that opens ChatGPT in an embedded browser and lets the user choose a local directory displayed in a filesystem tree.

**Architecture:** `MainWindow` composes a persistent-profile `QWebEngineView` with a standalone `DirectoryPane` in a horizontal splitter. `DirectoryPane` encapsulates `QFileSystemModel`, `QTreeView`, and directory-selection UI so later ZIP-selection behavior has a focused home.

**Tech Stack:** C++23, Qt 6 Widgets, Qt 6 WebEngineWidgets, Qt Test, CMake 3.16+

**Spec:** `docs/superpowers/specs/2026-09-12-chat-zip-stage-1-design.md`

## Global Constraints

- Build as C++23.
- Use Qt 6 Widgets and WebEngineWidgets.
- Use a single top-level CMake file with explicit source lists, following the supplied `stick_man` CMake style.
- Stage 1 contains no ZIP creation, upload automation, download interception, or patch application.

---

### Task 1: Directory pane

**Files:**
- Create: `src/ui/directory_pane.hpp`
- Create: `src/ui/directory_pane.cpp`
- Create: `tests/directory_pane_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ui::DirectoryPane`, `void setRootDirectory(const QString&)`, `QString rootDirectory() const`

- [ ] **Step 1: Write the failing test**

Create a Qt Test that constructs `DirectoryPane`, creates a `QTemporaryDir`, calls `setRootDirectory()`, and verifies the tree becomes enabled and resolves its root index to that directory.

- [ ] **Step 2: Run the test to verify it fails**

Configure and build with CMake, then run `ctest --test-dir build -R directory_pane --output-on-failure`. The test must fail before `DirectoryPane` exists, assuming the build environment contains Qt 6 development packages.

- [ ] **Step 3: Write the minimal implementation**

Implement `DirectoryPane` with `QPushButton`, `QLabel`, `QFileSystemModel`, and `QTreeView`. Connect the button to `QFileDialog::getExistingDirectory()`. `setRootDirectory()` canonicalizes the root, updates the model/tree root, label, and enabled state.

- [ ] **Step 4: Run the test to verify it passes**

Run `ctest --test-dir build -R directory_pane --output-on-failure`.

### Task 2: Main application shell

**Files:**
- Create: `src/ui/main_window.hpp`
- Create: `src/ui/main_window.cpp`
- Create: `src/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ui::DirectoryPane`
- Produces: `ui::MainWindow`

- [ ] **Step 1: Implement the application shell**

Create a named `QWebEngineProfile` in `main.cpp` so it outlives the window. Create `MainWindow` with a horizontal `QSplitter`, a left `QWebEngineView`, and a right `DirectoryPane`; associate its `QWebEnginePage` with the supplied profile, load `https://chatgpt.com/`, and set initial splitter/window sizing.

- [ ] **Step 2: Build the complete application**

Run `cmake --build build` and ensure `chat_zip` links successfully.

- [ ] **Step 3: Verify project hygiene**

Run `git diff --check` and inspect the project archive to ensure build products and temporary files are not included.
