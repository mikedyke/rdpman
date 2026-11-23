# rdpman

Small portable manager for Windows Remote Desktop sessions. A single native Win32 executable
(C++, static runtime) with no .NET or other runtime dependency. Connections are opened with the
built-in `mstsc.exe`.

## Features

- Session list with search and groups; connect, create, edit, duplicate, delete.
- Sessions are ordinary `.rdp` files in a `sessions` folder next to the exe (falls back to
  `%APPDATA%\rdpman` if that folder is read-only).
- Editor with tabs for the common settings plus an "All settings" tab listing about 100 known RDP
  properties; any other `name:type:value` line can be added.
- Monitor picker for full screen: shows the attached monitors in their real layout with the IDs
  `mstsc` uses (`mstsc /l`), first clicked monitor becomes the remote primary.
- Detects when the monitor setup no longer matches a session (monitor removed, resolution or
  position changed, monitors no longer adjacent) and warns in the list and before connecting.
- Import (menu, drag and drop, or copy into `sessions`) and export of plain `.rdp` files; unknown
  lines are preserved.
- `rdpman.exe "session name"` connects directly.

## Build

Requires Visual Studio (or Build Tools) with the C++ x64 toolset.

```
build.cmd
```

## Notes

- Requires Windows 10 or later.
- Saved passwords are encrypted with Windows DPAPI and only work for the same Windows user on the
  same PC.
- Group, notes, extra `mstsc` arguments and the stored monitor layout are kept as `rdpman ...`
  lines in the session file; they are stripped from exports and from the file passed to `mstsc`.
