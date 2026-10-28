# LightNote

LightNote is a lightweight, native Windows TXT editor built with C++/Win32 and Scintilla.

## Download for Windows

**[Download the latest Windows EXE](https://github.com/guoqingh/LightNote/releases/latest)**

**[Download LightNote.exe — v0.8.3](https://github.com/guoqingh/LightNote/releases/download/v0.8.3/LightNote.exe)**

Download `LightNote.exe` and double-click to start. No installer is required. On first launch, Windows SmartScreen may warn because this independent build is not digitally signed.

## Features (v0.8.3)

- Open multiple TXT files in a single main window and switch using a left-side document list.
- Resize the sidebar, drag to reorder documents, and drag TXT files from Explorer into either side.
- Open the same underlying TXT only once, even when opened repeatedly.
- Right-click a document in the sidebar to rename it, or use **F2**. The actual file is renamed, without auto-saving unsaved edits.
- Find/replace, line numbers, undo/redo, **manual** save (Ctrl+S), and separate recovery snapshots.
- Multi-color text highlighting (red, yellow, green, blue, cyan, indigo, purple) and bold text.
- Highlight metadata in `%LOCALAPPDATA%\LightNote\meta`, shared across program-version folders since v0.8.3.

## Notes

- This is a portable Windows EXE, not a signed installer. Run normally rather than *as administrator* if you want to drag files from Explorer.
- Recovery is best-effort and does not guarantee preservation of the final moments before sudden power loss.
- A text document's contents are **not** automatically saved; use Ctrl+S.
- Local documents, recovery data, backups, credentials, vendor archives and toolchains are not included in this repository.

## Source snapshot and building

Current public source snapshot: **v0.8.3 (2026-10-28)**.

The `src/main.cpp` source requires Windows, a compatible C++17 MinGW-w64 toolchain, Scintilla headers and a compatible Scintilla static library. The third-party Scintilla binaries and a fully reproducible standalone build environment are **not** included here.

## Third-party code

LightNote uses the Scintilla editing component. Refer to the component's upstream distribution and license terms.
