#!/usr/bin/env bash
# Cross-compile Strata.exe from Linux/macOS with Zig (pip install ziglang, or a zig binary on PATH).
set -euo pipefail
cd "$(dirname "$0")"
if [[ -z "${ZIG:-}" ]]; then
  if command -v zig >/dev/null 2>&1; then ZIG=zig; else ZIG="python3 -m ziglang"; fi
fi
mkdir -p dist
$ZIG c++ -target x86_64-windows-gnu -std=c++17 -O2 -s -municode \
  -DUNICODE -D_UNICODE \
  -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers \
  src/*.cpp res/strata.rc -o dist/Strata.exe \
  -Wl,--subsystem,windows \
  -ld2d1 -ldwrite -lole32 -loleaut32 -luuid -ldwmapi -lshcore -lshell32 -luser32 -lgdi32 -ladvapi32 -lshlwapi -lwindowscodecs -lcomctl32 -lmpr -luxtheme
rm -f dist/Strata.pdb
ls -la dist/Strata.exe
