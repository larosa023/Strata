# Strata

A fast, modern, keyboard-first File Explorer replacement for Windows.
Part of the larosa023 suite of lightweight Windows apps (Foyer, Faderly, Ember, Tonely, Mantel, Marq, Lumora…).

**Status: v0.1.1 (Phase 1)** — tabbed window, breadcrumb path bar, instant folder filter, whole-PC search (Everything or built-in scan), sidebar with
pinned folders and drives, a virtualized sortable file list, and open / copy / move / rename / delete
through the Windows copy engine.

---

## Run it

1. Download `Strata.exe` from [Releases](https://github.com/larosa023/Strata/releases) and put it anywhere
   (e.g. `C:\Apps\Strata\`). No installer, no admin rights, no runtime to install.
2. Double-click it. File Explorer is untouched — Strata runs alongside it.

Windows may show a SmartScreen warning the first time because the exe isn't code-signed yet:
**More info → Run anyway**.

Portable mode: create an empty file named `portable` next to `Strata.exe` and settings go in a `data`
folder beside it instead of `%APPDATA%\Strata`.

`Strata.exe "D:\Projects"` opens a folder in a new tab (in the running window if Strata is already open).

## Using Strata

| | |
|---|---|
| **Tabs** | Ctrl+T new · Ctrl+W close · Ctrl+Shift+T reopen closed · Ctrl+Tab / Ctrl+PgUp/PgDn switch · Ctrl+1…9 · drag to reorder · middle-click to close · right-click for Duplicate / Close others · mouse wheel over the tab strip |
| **Navigate** | Enter or double-click opens · Backspace or Alt+↑ goes up · Alt+← / Alt+→ (or mouse back/forward buttons) · middle-click or Ctrl+click a folder to open it in a new tab |
| **Path bar** | Click a segment to jump · click a **›** to pick a sibling folder · click empty space (or Ctrl+L / Alt+D) to type a path — `%USERPROFILE%`, `~`, relative paths, and a file path (opens its folder and selects it) all work |
| **Search** | Ctrl+K (or Ctrl+F). On **This PC** it searches every drive — instantly if [Everything](https://www.voidtools.com/) is running, otherwise with a background scan. In a folder it filters as you type; **Enter** searches all subfolders. `*` and `?` wildcards work · Esc clears |
| **Select** | Click, Ctrl+click, Shift+click, Ctrl+A, drag a box in empty space · type a name to jump to it |
| **Files** | F2 rename · Del → Recycle Bin · Shift+Del delete permanently · Ctrl+C / Ctrl+X / Ctrl+V (works with File Explorer both ways) · Ctrl+Shift+N new folder · Ctrl+Shift+C copy path · Alt+Enter properties |
| **View** | Click a column header to sort, drag its edge to resize, right-click it to choose columns · Ctrl+H show hidden items · F5 or Ctrl+R refresh |
| **Right-click** | The full Windows menu (Open with, Send to, 7-Zip, Properties…) plus Strata's own: Open in new tab, Pin to sidebar, Open terminal here, Copy path |
| **Sidebar** | Right-click a pin to give it a color tag, reorder, or unpin · drag the edge to resize |
| **⋯ menu** | Theme (Dark, Light, Midnight, Glass, Ember, Ocean, Follow Windows), background (Solid / Mica / Acrylic), row height, hidden items, extensions, folders-first, session restore |

Strata remembers your tabs, window size, column widths, and sort order between runs.

---

## Why this tech stack

Strata is **C++17 + Win32 + Direct2D/DirectWrite**, the same stack as Foyer, cross-compiled with Zig.

- **Speed:** a single ~0.5 MB native exe with no runtime. Starts in milliseconds and idles at ~0% CPU.
  The file list is custom-drawn and virtualized (only visible rows are drawn), so a folder with 100,000
  files scrolls as smoothly as one with 10.
- **Shell integration:** the real Windows APIs are called directly — `IContextMenu` for the right-click
  menu (including Send to, Open with, 7-Zip and other extensions), `IFileOperation` for copy/move/delete
  (Recycle Bin, conflict dialogs, Windows undo), the system image list for icons, and the OLE clipboard
  so cut/copy/paste interoperates with File Explorer. Thumbnails and drag-and-drop use the same COM APIs.
- **Matches the suite:** Foyer's theme engine (`gfx.cpp`), JSON settings, logging, and portable-mode
  conventions are shared, so theme files and the updater can be shared too.

Trade-offs considered:

| Option | Why not |
|---|---|
| C# + WinUI 3 / WPF | Nicer XAML tooling, but needs the .NET / Windows App SDK runtime, starts slower, uses 80–150 MB RAM, and shell COM interop is clunkier. |
| Electron / Tauri | Web UI is easy to theme but can't host the native context menu or shell drag-and-drop cleanly, and Electron is heavy. |
| Rust + windows-rs | Comparable speed, but no code sharing with Foyer and slower iteration on COM-heavy shell code. |

The cost of native C++ is that every control is hand-drawn — which is also what gives the suite its look.

## Project structure

```
src/
  main.cpp        entry point, single instance, command line
  window.cpp/.h   main window: title bar + tabs, toolbar, breadcrumb, sidebar, list, status bar, input
  fs.cpp/.h       background folder listing, drives, folder change watching, path helpers
  shell.cpp/.h    icons, Windows context menu, IFileOperation, clipboard, launching
  search.cpp/.h   search: Everything IPC when available, background scan otherwise
  textinput.*     self-drawn text field (search, path bar, rename)
  gfx.cpp/.h      Direct2D/DirectWrite helpers + theme presets (shared with Foyer)
  config.cpp/.h   settings, pins, session (config.json)
  json.h          tiny JSON reader/writer (shared with Foyer)
  log.cpp         strata.log + app data paths (shared with Foyer)
res/              icon, manifest, version resource
.github/workflows build + automatic GitHub Release when STRATA_VERSION changes
```

Coming in later phases: `queue.cpp` (transfer queue), `sizes.cpp` (background folder sizes),
`index.cpp` (Strata's own whole-PC index), `preview.cpp`, `tags.cpp`, `settings.cpp`, `updater.cpp`
(shared with Foyer), `archive.cpp`, `git.cpp`.

## Roadmap

- [x] **Phase 1** — window, tabs, toolbar, path bar, sidebar (pins + drives), single-pane browsing with sortable columns, open / copy / move / rename / delete
- [ ] **Phase 2** — dual-pane mode, full keyboard map (Tab, F5/F6 to other pane), transfer queue, undo, background folder sizes, drag and drop
- [ ] **Phase 3** — whole-PC search (own index or Everything), preview panel, color tags on files and folders
- [ ] **Phase 4** — theme editor and theme files (shared with Foyer), layout options, settings window, right-click menu cleanup
- [ ] **Phase 5** — built-in updater, installer + portable mode, set as default file manager with safe fallback, backup/restore
- [ ] **Phase 6** — animations, accessibility, performance tuning, archive browsing, Git status

## Build

On Linux/macOS: `pip install ziglang && ./build.sh` → `dist/Strata.exe`.
On Windows with Visual Studio 2022: `cmake -S . -B build -A x64 && cmake --build build --config Release`.

Every push to `main` builds the exe in GitHub Actions; bumping `STRATA_VERSION` in `src/common.h`
publishes a release (versions with a `-`, like `0.2.0-beta.1`, are published as pre-releases).

Logs: `%APPDATA%\Strata\strata.log` (⋯ menu → Open settings & log folder).
