## Strata 0.3.3

- **Fixed: updates wouldn't install while Strata was maximized.** The updater waits for full-screen games and videos to close before installing. A maximized Strata window also fills the screen, so Strata mistook itself for a game and kept postponing. Now:
  - Strata's own windows and normal maximized windows don't count as full-screen.
  - Clicking **Update** installs straight away. The only thing it still waits for is a file transfer in progress, and it tells you when that's the case.

Also in 0.3.2: menus drawn in Strata's theme (rounded, acrylic, icons, accent checks), and a tidier ⋯ menu with View and Appearance submenus.
