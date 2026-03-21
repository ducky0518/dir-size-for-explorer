# DirSize for Explorer

A Windows shell extension that displays **directory sizes** directly in Explorer's built-in **Size** column — no extra columns, no third-party file managers.

![Windows 10/11](https://img.shields.io/badge/Windows-10%2F11-0078D6?logo=windows) ![License: MIT](https://img.shields.io/badge/License-MIT-green) ![Architecture: x64](https://img.shields.io/badge/arch-x64-blue)

## How It Works

Windows Explorer normally leaves the Size column blank for folders. DirSize hooks into Explorer's rendering pipeline using [Microsoft Detours](https://github.com/microsoft/Detours) to fill in calculated directory sizes — transparently, with no UI changes to Explorer itself.

**Three-layer hook architecture:**

1. **NtQueryDirectoryFile** — injects size data into NT-level directory listings so Explorer sees folder sizes during file enumeration
2. **IPropertyStore::GetValue** — intercepts property queries for directories and returns cached sizes from the database
3. **IPropertyDescription::FormatForDisplay** — optionally auto-scales display formatting (KB → MB → GB) instead of Explorer's default KB-only display

A background Windows service handles the actual scanning and caches results in a local SQLite database. An optional system tray app provides a settings GUI.

## Features

- **Directory sizes in the Size column** — works in Details view, no extra columns needed
- **Background scanning service** — scans on a configurable interval with low IO priority
- **NTFS Change Journal** — efficient incremental updates when files change (no full rescan needed)
- **Size metrics** — choose between "Size" (logical) or "Size on disk" (cluster-rounded allocation)
- **Auto-scale formatting** — optionally show sizes as KB/MB/GB instead of Explorer's KB-only default
- **Configurable scope** — apply auto-scaling to folders only or files + folders
- **System tray settings app** — configure everything from a tabbed settings dialog
- **Low system impact** — configurable IO priority, batched database writes, in-memory LRU cache

## Installation

1. Download `DirSizeForExplorer.msi` from the [latest release](https://github.com/ducky0518/dir-size-for-explorer/releases/latest)
2. Run the installer (requires admin for service and shell extension registration)
3. Explorer restarts automatically to load the extension
4. Open **DirSize Settings** from the system tray or Start Menu to add watched directories

### Requirements

- Windows 10 or 11 (x64)
- NTFS volumes recommended for Change Journal support

## Configuration

All settings are accessible from the **DirSize Settings** tray app. Two tabs:

### Scanner Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Scan Interval | 30 min | How often to run a full scan (1–1440 minutes) |
| IO Priority | Low | Scanner disk priority: Very Low, Low, or Normal |
| Watched Directories | — | List of root directories to scan |
| Use Change Journal | On | Enable NTFS USN journal for incremental updates |

### Display Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Size Metric | Logical Size | "Size" (sum of file sizes) or "Size on disk" (cluster-rounded) |
| Column Formatting | Explorer Default | Explorer's native KB display, or auto-scaled KB/MB/GB |
| Auto-scale Scope | Folders only | Apply auto-scaling to folders only, or files + folders |

Settings are stored in the registry at `HKLM\SOFTWARE\DirSizeForExplorer` and take effect within 30 seconds (no restart needed).

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   Windows Explorer                   │
│                                                      │
│  NtQueryDirectoryFile ──► IPropertyStore::GetValue   │
│          │                        │                  │
│          ▼                        ▼                  │
│  ┌─────────────────────────────────────┐             │
│  │     DirSizeShellExt.dll (hooks)     │             │
│  │  ┌───────────┐  ┌────────────────┐  │             │
│  │  │ LRU Cache │  │ Format Hook    │  │             │
│  │  │ (10k, 60s)│  │ (auto-scale)   │  │             │
│  │  └─────┬─────┘  └────────────────┘  │             │
│  └────────┼────────────────────────────┘             │
└───────────┼──────────────────────────────────────────┘
            │ read-only
   ┌────────▼────────┐
   │   SQLite DB      │     ┌──────────────────────┐
   │  (dirsize.db)    │◄────│   DirSizeSvc.exe     │
   │  ProgramData     │     │  (background service) │
   └──────────────────┘     │                      │
                            │  Scanner + USN Journal│
                            └──────────────────────┘
                                       ▲
                            ┌──────────┤ IPC (named pipe)
                            │          │
                      ┌─────▼──────────┐
                      │ DirSizeTray.exe │
                      │ (settings GUI)  │
                      └─────────────────┘
```

### Components

| Component | Description |
|-----------|-------------|
| **DirSizeShellExt.dll** | Shell extension loaded into Explorer — hooks APIs via Detours, reads cached sizes from SQLite |
| **DirSizeSvc.exe** | Windows service — recursively scans watched directories, computes sizes, writes to database |
| **DirSizeTray.exe** | System tray app — settings GUI, communicates with service via named pipe IPC |
| **DirSizeCommon.lib** | Shared library — config, database, IPC protocol, GUIDs |

## Building from Source

### Prerequisites

- **Visual Studio 2022** (or Build Tools) with C++ desktop workload
- **CMake 3.24+**
- **vcpkg** (for dependency management)

### Dependencies

Managed via `vcpkg.json`:
- [sqlite3](https://www.sqlite.org/) — size cache database
- [Microsoft Detours](https://github.com/microsoft/Detours) — API hooking

### Build Steps

```powershell
# Clone
git clone https://github.com/ducky0518/dir-size-for-explorer.git
cd dir-size-for-explorer

# Configure (uses CMakePresets.json — requires VCPKG_ROOT set)
cmake --preset default

# Build Release
cmake --build build --config Release

# Build MSI installer (requires WiX v4+ and .NET SDK)
dotnet tool install --global wix
wix extension add WixToolset.Util.wixext
wix extension add WixToolset.UI.wixext
wix build installer/Product.wxs installer/Components.wxs `
    -d BuildDir=build/shell-ext/Release `
    -o build/DirSizeForExplorer.msi `
    -arch x64 `
    -ext WixToolset.Util.wixext `
    -ext WixToolset.UI.wixext
```

### Build Presets

| Preset | Config | Description |
|--------|--------|-------------|
| `default` | Release | Optimized build |
| `debug` | Debug | Debug symbols, assertions |

## Uninstalling

Use **Add/Remove Programs** (Settings → Apps) or run:

```powershell
msiexec /x DirSizeForExplorer.msi
```

The installer handles COM unregistration, service removal, property schema cleanup, and Explorer restart.

## How It's Different

| Approach | Drawback |
|----------|----------|
| TreeSize / WinDirStat | Separate app, not integrated into Explorer |
| Folder Size (column handler) | Adds a custom column — not the native Size column |
| Explorer++ / Directory Opus | Replaces Explorer entirely |
| **DirSize for Explorer** | **Fills in Explorer's own Size column, zero UI changes** |

## License

[MIT](LICENSE)

## Acknowledgments

- [Microsoft Detours](https://github.com/microsoft/Detours) — inline API hooking library
- [SQLite](https://www.sqlite.org/) — embedded database engine
- [WiX Toolset](https://wixtoolset.org/) — MSI installer framework
