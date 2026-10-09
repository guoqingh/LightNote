# LightNote

Lightweight native Windows text editor based on Win32 and Scintilla.

## Features

- UTF-8 text editing and line numbers
- Find/replace, undo/redo, manual file saving
- Recovery snapshots for unexpected termination
- Selection text colors (red, yellow, green, blue, cyan, indigo, purple) and bold formatting
- Custom blue-and-white application icon

## Version

Source snapshot: V0.7.2 (2026-10-09).

## Build requirements

Windows, a C++17 MinGW-w64 toolchain, and a compatible Scintilla static library / headers. `src/main.cpp` uses Scintilla API and links against its library; these third-party binaries are **not** bundled in this public source snapshot.

This repository intentionally excludes local editor documents, recovery snapshots, local backups, bundled toolchains, and third-party vendor archives. A reproducible build workflow has not yet been added.

## Privacy

No desktop TXT files or user data are included.

## Third-party components

Scintilla is a third-party text editing component. See its own license and official distribution for source and licensing terms.
