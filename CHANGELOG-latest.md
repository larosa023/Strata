## Strata 0.2.0 — the big one

Everything from the remaining roadmap phases lands in this release.

**Dual pane & keyboard**
- Dual-pane layout (default) with **Tab** to switch panes, **F5** copy / **F6** move to the other pane, and a view switch for dual, single, and grid.
- Grid view with thumbnails. Strata remembers sort order, view, and columns for each folder.
- Columns adapt to narrow panes.

**Transfers**
- Strata's own transfer queue: pause, resume, cancel, and skip, with speed and ETA. **Ctrl+J** opens the transfers panel.
- Optional SHA-256 **verify after copy**. Strata warns when the destination is low on space and lets you set a default for name conflicts.
- **Ctrl+Z undo** for rename, move, copy, new folder, and delete (deleted items come back from the Recycle Bin).
- **Batch rename:** press F2 with several items selected. You can find and replace (with optional regex), add a prefix or suffix, number items, and change case, with a live preview. Ctrl+Z undoes the whole batch.
- Drag and drop between panes, tabs, the sidebar, and other apps.

**Folder sizes & tags**
- Folder sizes are calculated in the background, with size bars. This pauses while you're gaming or the CPU is busy.
- Color tags on files and folders: Urgent, Active, Done, and Repo, plus your own. Click a tag in the sidebar to see everything with that tag.

**Search**
- Search uses **Everything** if it's running; otherwise it uses Strata's own whole-PC index, which rebuilds in the background.
- Filters: `type:pdf`, `ext:dwg`, `size:>50mb`, `modified:7d`, `modified:2026-03`, `tag:urgent`, `folder:`, and `file:`.
- **Saved searches** become smart folders in the sidebar.

**Preview panel** (Ctrl+P, or Space for a quick look)
- Images, text and code, PDF, Office files, and video through the Windows preview handlers.
- File details, plus "what's taking space" for folders.
- **Git status** (branch, changes, ahead/behind), with buttons for Open and Open in other pane.

**Archives:** browse into .zip files like folders, and extract with drag-out or **Extract here**.

**Right-click menu:** a cleaner menu that hides clutter you choose (hold **Shift** to see everything), plus Open in other pane, Calculate size, Copy path, Open terminal, and Add tag.

**Settings window** (Ctrl+,): General, Layout, Themes, Search, Tags, File operations, Updates, and About.
- Theme presets and a custom theme with live preview, plus accent color, Mica or Acrylic backdrop, font, size, corner radius, and density.
- Theme files can be imported and exported, and are compatible with Foyer.

**System**
- **Built-in updater:** Stable and Beta channels, SHA-256 check, and automatic rollback if a new version fails to start. It never installs during a transfer or while you're gaming. Future versions install themselves.
- Install to `%LOCALAPPDATA%\Programs` (Start menu entry, uninstall from Apps) or run in portable mode.
- Start with Windows, with a tray icon and close to tray.
- Optional **default file manager** (folders and Win+E open in Strata) with a safe fallback. If Strata crashes or hangs, folders open in File Explorer, and `Strata.exe --restore-explorer` undoes it.
- Back up and restore settings, pins, tags, and folder views as one file.
- Respects Windows high-contrast mode and text scaling.
