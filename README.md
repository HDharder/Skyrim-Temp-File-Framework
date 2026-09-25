# Temp File Framework

An SKSE plugin (CommonLibSSE-NG) that lets any mod create **session-only temp files** that the
game treats as if they were in `Data`. Originals are never touched, and nothing goes into the
game's `Data` folder or MO2's `overwrite`.

```
A mod asks: Copy("SKSE/Plugins/MyMod/config.json")
Framework:  copies the visible original (loose file through the MO2 VFS, or from inside a BSA)
            -> %TEMP%\SkyrimTempFiles\<pid>-<creation>\files\SKSE\Plugins\MyMod\config.json
From then on, EVERY read/write of Data\SKSE\Plugins\MyMod\config.json
(the game engine, std::ifstream, std::filesystem, another SKSE plugin...) lands in the temp file.
Delete(...) -> the game sees the original again.
Game closes / CTD / process killed -> Windows deletes the temp files.
```

## Operations

| Operation | Behavior |
|---|---|
| `Copy(path)` | Creates the temp file from the original. **If it already exists** (created by you or by another mod), returns `kTempFile_AlreadyExists` and **does not copy again**. |
| `Create(path, data, size)` | Creates the temp file with this content, or **replaces** the content of the existing one. The original does not need to exist (brand-new files, e.g. a `.nif`). The engine sees it at any time, even over a file inside a BSA (see [How it works](#how-it-works)). |
| `Delete(path)` | Deletes the temp file; the original applies again. |
| `Exists(path)` | Is there a temp file for this path? |
| `GetRealPath(path, out, size)` | Physical path of the temp file (rarely needed - writing to the normal Data path already reaches it). |

Paths: relative to `Data`, `/` or `\`, a leading `Data/` is optional, case-insensitive. `..` and
absolute paths are rejected.

## Using it from another SKSE plugin

Copy [`include/TempFileAPI.h`](include/TempFileAPI.h) into your project. Nothing to link - if the
framework is not installed, `GetAPI()` returns `nullptr`.

```cpp
#include "TempFileAPI.h"

// kPostLoad or later
if (const auto* tf = TempFile::GetAPI()) {
    tf->Copy("SKSE/Plugins/MyMod/config.json");       // creates (or reuses) the copy

    std::ofstream("Data/SKSE/Plugins/MyMod/config.json") << newContent;  // writes TO THE TEMP FILE

    const std::string nif = BuildMesh();
    tf->Create("meshes/MyMod/temp.nif", nif.data(), nif.size());         // brand-new file

    tf->Delete("SKSE/Plugins/MyMod/config.json");     // the original applies again
}
```

## Using it from Papyrus

The compiled script ships with the mod (`Scripts/TempFile.pex`); the source is
[`Scripts/Source/TempFile.psc`](Scripts/Source/TempFile.psc). Only this framework's developers
need the Creation Kit: `tools\compile_papyrus.ps1` rebuilds the `.pex` after the `.psc` changes.

```papyrus
string real = TempFile.Copy("SKSE/Plugins/MyMod/config.json")
TempFile.Create("SKSE/Plugins/MyMod/state.txt", "content")
TempFile.Delete("SKSE/Plugins/MyMod/state.txt")
```

## Session-only paths (for players and modlists)

Optional, **off by default**. Paths listed here never get written to disk: when any mod writes to
a matching path, the file becomes a session-only temp file first, so nothing lands in `Data` or in
MO2's `overwrite` folder, and it is gone when the game closes. This works for **any** mod - it does
not need to know about the framework.

`Data\SKSE\Plugins\TempFileFramework.ini`:

```ini
[SessionOnly]
SKSE/Plugins/SomeMod/cache/*
SKSE/Plugins/*.log
```

- One path per line, relative to `Data`; `*` matches anything (subfolders included), `?` one
  character; case does not matter.
- Mods and modlists can ship their own list without touching the player's file: any `.ini` with a
  `[SessionOnly]` section in `Data\SKSE\Plugins\TempFileFramework\SessionOnly\`.
- Writing an existing file starts from its current content (appending works), and the file on disk
  keeps its original content. Folders created under these paths only exist in the session too.
- **Only list files that are safe to lose every session** - caches, logs, data rebuilt at
  startup. Settings and saves (MCM settings, PapyrusUtil/JContainers data, RaceMenu presets...)
  would be wiped every time the game closes.
- It prevents new clutter; it does not repair an `overwrite` folder that is already broken.

## How it works

**Redirection** - hooks (MinHook) on the Windows file API in `kernelbase`, which cover the whole
process: `CreateFile`, `CreateFile2` (MSVC's `std::filesystem` opens files through it),
`GetFileAttributes(Ex)`, `FindFirstFile(Ex)`/`FindNextFile`/`FindClose` (with merged listings: a
file that only exists as a temp file shows up when listing its folder), `DeleteFile`,
`MoveFileEx`, `ReplaceFile`, `CopyFile(Ex)`, `CopyFile2` and `SetFileInformationByHandle`
(delete/rename through a handle - that is how `std::filesystem::remove` deletes). A and W versions.

**The engine's resource index** - redirecting file access is not enough for the game itself.
While loading the archives, the engine builds an index of every resource (file ID -> archive
record or loose-file record), and its model loader resolves paths **only** through that index -
never through the disk. A path that was in a BSA at startup stays a BSA path, and a path that did
not exist at startup is never found. So, for every temp file, the framework updates that index at
runtime exactly the way the game does at startup, with the game's own functions and under the
index lock:

- a **BSA record** is converted into a **loose record**; deleting the temp file puts the original
  BSA record back;
- a **brand-new path** gets a new loose record;
- a path that is already loose (e.g. a mod's loose file) needs nothing - its loose record opens
  the Data path, which the file API hooks redirect.

The engine functions are found through Address Library IDs, and the instructions around them are
checked at startup; on a runtime whose code does not match (currently anything but 1.6.x) this
part turns itself off and a temp file over a BSA-only path returns `kTempFile_ArchiveLocked`.
How the index works was worked out from call stacks captured in game and a disassembly of the
running (decrypted) executable - the notes are in `src/ArchiveBypass.cpp`.

**Coexisting with MO2** - our hook sits in front of usvfs. We redirect the `Data` path to
`%TEMP%`, and usvfs lets it through because it is not a `Data` path. To read the original, the
framework calls the original function, which still goes through usvfs, so it sees exactly the
file the VFS shows. Vortex and manual installs work the same way (no usvfs in between).

**Protecting the original** - temp files live **outside** the game folder, so they never compete
with mod files in the VFS. Deleting, moving or copying over a path that has a temp file touches
the temp file, never the original. That includes the "write `x.tmp` and rename it over `x.json`"
pattern.

**Cleanup (3 layers)**
1. **CTD / process killed** - the framework keeps every temp file open with
   `FILE_FLAG_DELETE_ON_CLOSE`. When the process dies, however it dies, the **kernel** closes the
   handle and deletes the file. This does not depend on a crash handler: it works even on stack
   overflow or "End task", cases where a CrashLogger-style handler never runs.
2. **Normal exit** - Skyrim quits by calling `TerminateProcess` on itself (DLLs never get
   `DLL_PROCESS_DETACH`), so the framework hooks that call and deletes the session folder right
   before the process ends. `DLL_PROCESS_DETACH` covers any other way out.
3. **Game startup** - deletes the folders of sessions whose process no longer exists. Each
   session is named `<pid>-<creation time>`, so a reused PID or two Skyrims running at once
   never get mixed up.

Log: `Documents\My Games\Skyrim Special Edition\SKSE\TempFileFramework.log`.

**Diagnostics** - `TempFile_DebugTrace(const char* filter)` (exported, not part of `TempFileAPI`)
logs every file call whose path contains `filter`, at the Win32 and the ntdll layer, to that log.
`nullptr` or `""` turns it off. The ntdll hooks are only installed the first time it is used.

## Known limitations

- **Engine cache**: once the game has loaded a `.nif`/`.dds`, changing the temp file does not
  reload what is already in memory. Create/copy it **before** the resource is loaded.
- **Engine index support needs runtime 1.6.x** (tested on 1.6.1170). On other runtimes a temp
  file over a BSA-only path returns `kTempFile_ArchiveLocked`: it exists for std/Win32 access, but
  the engine keeps the BSA copy. Everything else works the same.
- **Copy from a BSA** only works from `kDataLoaded` on (the archives must be loaded to read them).
- A temp file created **before the archives load** (`kPostLoad`) over a BSA path is registered by
  the game itself as a loose file, so no BSA record is ever created for it: if it is deleted later,
  the engine cannot fall back to the BSA copy until the game restarts. Temp files created after
  `kDataLoaded` restore the BSA copy normally.
- An **external** process (e.g. opening the temp file in Notepad++ while the game runs) may get
  "file in use" if it does not ask for `FILE_SHARE_DELETE`.
- Writing a **new** file inside a folder that only exists because of temp files (it does not
  exist in the real `Data`) fails: use `Create` for new files.
- Code that opens files directly through `NtCreateFile` (below Win32) is not redirected. The
  game, the CRT and `std::filesystem` all use Win32.

## Out-of-game test

`tests\run_tests.bat` builds the real `Store.cpp` + `Hooks.cpp` into a plain .exe (the .exe's
folder plays the part of the game folder) and checks 96 cases: redirection through
`ifstream`/`ofstream`, `std::filesystem`, Win32 A/W, folder listings, "save to .tmp + rename",
`copy_file`, `fs::remove`, deleting while the file is open, invalid paths, session-only paths, **CTD** and **exit**.
In the CTD test a child process creates a temp file and is killed without running any of our
code; the test checks that Windows deleted the file and that the next launch's sweep deletes the
folder. In the exit test a child quits the way Skyrim does (`TerminateProcess` on itself) and the
whole session folder must be gone.

`tests\ingame\` is the in-game counterpart (`TempFileTester.dll`, built alongside, never shipped):
it runs on the first frame after `kDataLoaded` and checks the MO2 VFS, BSAs and the engine's own
resource loader, writing `TempFileTester.log`. It also points four iron weapons at temp files that
hold the sweet roll model - early and late, over BSA files and over brand-new paths - so the
game's model loader can be checked by eye (`player.additem 0001397E 1`, `00012EB7`, `00013982`,
`00013983`: all four must look like a sweet roll).

## Build

CMake + vcpkg (`commonlibsse-ng`, `minhook`), `debug`/`release` presets, `x64-windows-static`
triplet. With `SKYRIM_MODS_FOLDER` set, the build copies the output to
`<mods>\TempFileFramework\SKSE\Plugins\`.

`tools\package_release.ps1 -Version x.y.z` turns a Release build into the install-ready
`dist\TempFileFramework-x.y.z.zip` (DLL + compiled script + script source). It ships no PDB,
strips source paths that prebuilt libraries embed in the DLL, and refuses to package if anything
tied to the build machine (project folder, user or computer name) is left in the files.

## License

- The framework (`src/`, `Scripts/TempFile.pex`, `tools/`, `tests/`) is licensed under the
  **GNU General Public License v3.0** - see [LICENSE](LICENSE). Modified versions and forks must
  keep the copyright notice and stay GPL-3.0, with their source code available.
- **[`include/TempFileAPI.h`](include/TempFileAPI.h) and [`Scripts/Source/TempFile.psc`](Scripts/Source/TempFile.psc)
  are MIT licensed** (the license text is in each file), so any mod - open or closed source - can
  include the header or compile scripts against the framework. Using the framework through them
  does not put your mod under the GPL.
- `TempFileFramework.dll` statically links CommonLibSSE-NG, MinHook, spdlog, {fmt}, Xbyak and
  rapidcsv; their notices are in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and ship with
  every release.

Copyright (c) 2026 HDharder
