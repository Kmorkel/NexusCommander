# NexusCommander
A fast, multithreaded dual-pane file manager for Windows — inspired by Total Commander, built with Qt6 and C++17
![NexusCommander](screenshots/nexuscommander.png)

## Features

- **Dual-pane layout** with tabbed browsing (Ctrl+T / Ctrl+W)
- **F-key shortcuts** — F5 Copy, F6 Move, F7 New Folder, F8 Delete, F2 Refresh
- **Background file operations** with a dedicated Job Queue dialog
  - Real-time progress, speed (MB/s), and ETA per job
  - Pause / Resume / Cancel individual jobs or the entire queue
  - Conflict resolution: Overwrite, Skip, Keep Both, with "Apply to all"
  - Global speed limiter (adjustable at any time, even mid-copy)
  - Optional "Shutdown PC when all jobs finish"
- **Clipboard integration** — Ctrl+C / Ctrl+X / Ctrl+V interoperates with Windows Explorer
- **Drive bar** with automatic USB hot-plug detection
- **Filter bar** (Ctrl+S) — wildcard filtering (e.g. `*.txt`, `image*`)
- **Navigation history** — Back / Forward buttons, mouse thumb buttons, Alt+Left/Right
- **Cross-partition move** — automatic copy-then-delete fallback with rollback on failure
- **Smart conflict handling** — stash-and-rename strategy prevents data loss on failed moves

## Download

Pre-built Windows binaries are available on the [Releases](../../releases) page — no Qt installation required.

## Build from Source

### Requirements

- **Qt 6.x** with the Widgets module (install via [qt.io/download-open-source](https://www.qt.io/download-open-source))
- **CMake 3.16+**
- **C++17** compiler (MSVC, MinGW, or GCC)

### Option 1 — Qt Creator

Open `CMakeLists.txt` directly in Qt Creator, select your Kit, and build. Works out of the box with both MinGW and MSVC kits.

### Option 2 — Visual Studio (MSVC)

If Qt is installed to the default location (`C:\Qt\`), CMake **auto-detects** the newest MSVC kit — no manual path needed:

```bash
git clone https://github.com/Kmorkel/NexusCommander.git
cd NexusCommander
cmake -B build
cmake --build build --config Release
```

CMake scans `C:\Qt\*\msvc*` and picks the newest 64-bit installation. If Qt is installed elsewhere, you can point to it explicitly:

```bash
cmake -B build -DCMAKE_PREFIX_PATH="D:/Qt/6.8.0/msvc2022_64"
```

Or set the environment variable `QT_ROOT` or `QTDIR` before running CMake.

### Option 3 — MinGW (command line)

MinGW is auto-detected the same way as MSVC — CMake scans `C:\Qt\*\mingw*` for the newest 64-bit kit:

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

Or pass the path explicitly:

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/mingw_64"
```

### Automatic Deployment (`deploy/` folder)

After a successful build — **whether MSVC or MinGW, Debug or Release** — `CMakeLists.txt` runs `windeployqt` as a post-build step and assembles a **fully self-contained build** under:

```
build/deploy/
 ├── NexusCommander.exe      ← the shippable executable
 ├── Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, ...
 ├── platforms/, styles/, imageformats/, ...
 └── (MinGW builds also include libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll)
```

This folder is the **real, distributable build** — everything needed to run the app on any Windows machine without a Qt installation. You can zip it up and hand it out as-is. `windeployqt` picks the matching runtime automatically based on the exe's compiler format, so MinGW and MSVC builds both produce correct, runnable output.

> The plain `build/<Config>/NexusCommander.exe` next to the build artifacts is the raw linker output used by the IDE for debugging; for anything you want to ship, use `build/deploy/`.

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| F2 | Refresh |
| F5 | Copy to other panel |
| F6 | Move to other panel |
| F7 | New Folder |
| F8 / Del | Delete |
| Tab | Switch between panels |
| Ctrl+T | New Tab |
| Ctrl+W | Close Tab |
| Ctrl+C / Ctrl+X | Copy / Cut to clipboard |
| Ctrl+V | Paste from clipboard |
| Ctrl+S | Focus filter bar |
| Ctrl+A | Select all |
| Backspace | Go to parent directory |
| Alt+Left / Right | Back / Forward |
| Middle-click (file list) | Open folder in new tab |
| Middle-click (tab bar) | Close tab |

## Architecture

```
MainWindow
 +-- QSplitter
 |    +-- QTabWidget (left)    -- one or more FilePanel tabs
 |    +-- QTabWidget (right)   -- one or more FilePanel tabs
 +-- FileWorkerThread          -- background thread, sequential job processing
 +-- JobQueueWidget            -- non-modal dialog with JobModel (QAbstractTableModel)
```

- **FilePanel** — address bar, drive bar, QListView (QFileSystemModel + filter proxy), filter bar
- **FileWorkerThread** — owns the job queue, runs on a dedicated QThread. Recursive copy in 1 MB chunks, adaptive chunk sizing, pause/resume via QWaitCondition
- **JobModel** — QAbstractTableModel that owns all job state; the view never touches items directly

All worker-to-UI communication goes through queued signal/slot connections. Conflict resolution intentionally blocks the worker thread via QWaitCondition until the user responds.

## Documentation

All public classes and methods carry Doxygen-style comments. If you have [Doxygen](https://www.doxygen.nl/) installed, you can generate a full HTML reference from the source:

```bash
doxygen -g Doxyfile      # once: create a default config
doxygen Doxyfile         # generate HTML under html/index.html
```

Open `html/index.html` in your browser to browse the class hierarchy, signal/slot graphs, and inline source.

## License

NexusCommander is licensed under the [MIT License](LICENSE).

### Third-Party

NexusCommander uses the [Qt Framework](https://www.qt.io/) (v6), which is licensed under the [LGPL v3](https://www.gnu.org/licenses/lgpl-3.0.html). Qt is linked dynamically — no modifications were made to the Qt source code. The Qt source code is available at [qt.io/download-open-source](https://www.qt.io/download-open-source).
