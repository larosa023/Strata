# Strata

A fast, modern, keyboard-first File Explorer replacement for Windows.
Part of the larosa023 suite of lightweight Windows apps (Foyer, Faderly, Ember, Tonely, Mantel, Marq, Lumora…).

**Status: v0.3.0.** Strata has:
- dual pane with F5/F6, a transfer queue with undo, and batch rename
- whole-PC search with filters and saved searches
- folder sizes and a **Space treemap**, color tags, and **version stacking** for `rev1/rev2/rev3` files
- **What changed since my last visit**, a **quick look carousel**, **Jump to folder**, a **focus filter**, and **locked and colored tabs**
- a preview panel with Git status, and archive browsing
- themed dialogs, animations, and transfer-finished chimes and notifications
- a built-in updater, installer or portable mode, and an optional default file manager mode

---

## Run it

1. Download `Strata.exe` from [Releases](https://github.com/larosa023/Strata/releases) and double-click it.
   It runs from anywhere. To install it properly, open **Settings → About → Install**. That copies it to
   `%LOCALAPPDATA%\Programs\Strata`, adds a Start menu entry, and lists it in Apps for uninstalling (no admin rights needed).
2. File Explorer is untouched unless you turn on **Settings → General → Use Strata as the default file manager**.

Windows may show a SmartScreen warning the first time because the exe isn't code-signed yet:
**More info → Run anyway**.

**Updates:** Strata checks GitHub Releases. When an update is ready, a pill appears in the title bar; click it to see
what's new and install. Downloads are verified with SHA-256. If the new version doesn't start, Strata rolls back
automatically. It never updates during a transfer or while a full-screen game is running. Choose Stable or Beta in
**Settings → Updates**.

**Portable mode:** use **Settings → About → Portable mode**, or create an empty file named `portable` next to `Strata.exe`.
Settings then go in a `data` folder beside it.

**Safe fallback:** if Strata is the default file manager and crashes or stops responding, folders open in File Explorer.
After 3 crashes in 10 minutes, Strata turns the default off by itself. To undo it by hand, run `Strata.exe --restore-explorer`.

Command line: `Strata.exe "D:\Projects"` opens a folder in a new tab · `--tray` starts in the tray · `--quit` · `--uninstall`.

## Using Strata

| | |
|---|---|
| **Panes** | **Tab** switches panes · **F5** copy / **F6** move the selection to the other pane · Ctrl+\\ dual / single · Ctrl+Shift+G grid · **Ctrl+Shift+S Space view** · Ctrl+P preview panel · Ctrl+B sidebar |
| **Jump** | **Ctrl+G** opens a fuzzy search over the folders you use, most frequent and recent first. Type "smi bid" for `D:\Bids\2026\Smith Library`. Enter goes there, Ctrl+Enter opens a new tab, Shift+Enter uses the other pane |
| **Quick look** | **Space** opens a big preview; **← →** flip through the folder; Enter opens the file; Space or Esc closes |
| **Focus** | **Ctrl+Shift+F** shows only PDFs, drawings, Office files, images, recent changes, a tag, or the same type as the selection. The filter stays while you browse; Esc or the chip's ✕ shows everything again |
| **What changed** | **Ctrl+Shift+H** lists every file in this folder and its subfolders changed since your last visit, today, this week, this month, or since a date |
| **Versions** | `Plans_rev1/rev2/rev3.pdf`, `(2)`, `- Copy` and dated copies stack under the newest. Click "3 versions", or press → / ←, to expand or collapse |
| **Tabs** | Ctrl+T new · Ctrl+W close · Ctrl+Shift+T reopen closed · Ctrl+Tab / Ctrl+1…9 · drag to reorder · right-click → **Lock tab to this folder** or **Tab color** |
| **Navigate** | Enter or double-click opens (zips open like folders) · Backspace or Alt+↑ goes up · Alt+← / Alt+→ · Ctrl+L types a path · type a name to jump to it |
| **Search** | Ctrl+K. In a folder, typing filters and **Enter** searches subfolders. On This PC it searches every drive: through [Everything](https://www.voidtools.com/) if it's running, otherwise through Strata's own index. Right-click → **Save search** for a smart folder |
| **Files** | F2 rename (with several selected: **batch rename** with live preview) · Del → Recycle Bin · Shift+Del delete · Ctrl+C/X/V · **Ctrl+Z undo** · Ctrl+Shift+N new folder · Ctrl+Shift+C copy path · Alt+Enter properties · drag and drop anywhere |
| **Transfers** | Ctrl+J opens the queue (pause, resume, skip, cancel, speed and ETA). Long transfers end with a soft chime and an "Open folder" card |
| **Tags** | Right-click → Tag. Click a tag in the sidebar to see everything with it |
| **Sidebar** | Drag pins, drives, tags, saved searches or whole section headers to rearrange them. Click a header to collapse it |
| **Settings** | Ctrl+, opens General, Layout (including animations and version stacking), Themes, Search, Tags, File operations (including sounds and notifications), Updates and About |

### Search filters

```
invoice type:pdf size:>2mb modified:30d
*.dwg folder:Projects
tag:urgent
report modified:2026-03
```

`type:` and `ext:` take extensions (`type:jpg,png`).
`size:` works with `<`/`>` and kb, mb or gb. `modified:` takes `today`, `7d`, `2026` or `2026-03`.
`tag:` takes a tag name. `folder:` and `file:` limit results to folders or files.

Strata remembers your tabs and window between runs, and each folder's view, sort and columns.

### Themes

Settings → Themes offers presets (Dark, Light, Midnight, Glass, Ember, Ocean, Follow Windows) and a **Custom** theme
built on any preset with your own colors. You can also set the accent, the Mica or Acrylic backdrop, the font, the
size, the corner radius and the density, with a live preview. **Export** saves a theme file that
Foyer can import, and **Import** reads Strata theme files or Foyer's `config.json`.

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
  main.cpp          entry point, single instance, command line, crash fallback to Explorer
  window.cpp/.h     main window: title bar + tabs, toolbar, panes, sidebar, preview panel, status bar, input
  fs.cpp/.h         background folder listing, archives, drives, folder watching, path helpers
  shell.cpp/.h      icons, Windows context menu (+ cleanup), IFileOperation, clipboard, launching
  queue.cpp/.h      transfer queue: CopyFileEx engine, pause/skip/cancel, verify, speed/ETA
  batch.cpp/.h      batch rename with live preview
  dialogs.cpp/.h    themed dialogs (conflicts, confirmations, prompts) + theming for classic dialogs
  visits.cpp/.h     folder visit history: Jump to folder and "since my last visit"
  versions.cpp/.h   version stacking (rev1 / rev2 / Rev B / dated copies)
  treemap.cpp/.h    squarified treemap layout for the Space view
  sizes.cpp/.h      background folder sizes (pauses when busy or gaming)
  tags.cpp/.h       color tags on files and folders
  dragdrop.cpp/.h   OLE drag and drop
  preview.cpp/.h    preview panel: images, text, preview handlers, thumbnails, Git status
  search.cpp/.h     search: Everything IPC, Strata's index, or a background scan
  query.cpp/.h      search filter syntax
  index.cpp/.h      Strata's own whole-PC file index
  settings.cpp/.h   settings window
  system.cpp/.h     install/uninstall, portable, start with Windows, tray, default file manager + crash guard
  updater.cpp/.h    GitHub Releases updater (shared with Foyer)
  autoupdate.*      Strata's updater glue (defers during transfers/games)
  textinput.*       self-drawn text field
  gfx.cpp/.h        Direct2D/DirectWrite helpers + themes (shared with Foyer)
  config.cpp/.h     settings, pins, tags, saved searches, folder views, backup, theme files
  json.h, log.cpp   shared with Foyer
res/                icon, manifest, version resource
.github/workflows   build + automatic GitHub Release when STRATA_VERSION changes
```

## Roadmap

- [x] **Phase 1** — window, tabs, toolbar, path bar, sidebar, sortable columns, file operations
- [x] **Phase 2** — dual pane, keyboard map, transfer queue, undo, folder sizes, drag and drop, batch rename
- [x] **Phase 3** — whole-PC search (own index or Everything) with filters and saved searches, preview panel, color tags
- [x] **Phase 4** — settings window, themes and theme files shared with Foyer, layout options, right-click menu cleanup
- [x] **Phase 5** — built-in updater, installer and portable mode, default file manager with safe fallback, backup/restore
- [x] **Phase 6** — archive browsing, Git status, high contrast and text scaling, grid view
- [x] **0.3** — version stacking, what changed since, Space view, quick look carousel, Jump to folder, focus filter, locked and colored tabs, animations, themed dialogs, transfer notifications
- [ ] Next: screen-reader (UI Automation) support, a network locations section in the sidebar

## Build

On Linux/macOS: `pip install ziglang && ./build.sh` → `dist/Strata.exe`.
On Windows with Visual Studio 2022: `cmake -S . -B build -A x64 && cmake --build build --config Release`.

Every push to `main` builds the exe in GitHub Actions; bumping `STRATA_VERSION` in `src/common.h`
publishes a release (versions with a `-`, like `0.2.0-beta.1`, are published as pre-releases).

Logs: `%APPDATA%\Strata\strata.log` (⋯ menu → Open settings & log folder).
