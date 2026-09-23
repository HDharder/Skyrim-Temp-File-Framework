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
| `Create(path, data, size)` | Creates the temp file with this content, or **replaces** the content of the existing one. The original does not need to exist (brand-new files, e.g. a `.nif`). |
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

[`Scripts/Source/TempFile.psc`](Scripts/Source/TempFile.psc) (compile it with the Creation Kit compiler):

```papyrus
string real = TempFile.Copy("SKSE/Plugins/MyMod/config.json")
TempFile.Create("SKSE/Plugins/MyMod/state.txt", "content")
TempFile.Delete("SKSE/Plugins/MyMod/state.txt")
```

## How it works

**Redirection** - hooks (MinHook) on the Windows file API in `kernelbase`, which cover the whole
process: `CreateFile`, `CreateFile2` (MSVC's `std::filesystem` opens files through it),
`GetFileAttributes(Ex)`, `FindFirstFile(Ex)`/`FindNextFile`/`FindClose` (with merged listings: a
file that only exists as a temp file shows up when listing its folder), `DeleteFile`,
`MoveFileEx`, `ReplaceFile`, `CopyFile(Ex)`, `CopyFile2` and `SetFileInformationByHandle`
(delete/rename through a handle - that is how `std::filesystem::remove` deletes). A and W versions.

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
2. **Normal exit** - `DLL_PROCESS_DETACH` closes everything and deletes the session folder.
3. **Game startup** - deletes the folders of sessions whose process no longer exists. Each
   session is named `<pid>-<creation time>`, so a reused PID or two Skyrims running at once
   never get mixed up.

Log: `Documents\My Games\Skyrim Special Edition\SKSE\TempFileFramework.log`.

## Known limitations

- **Engine cache**: once the game has loaded a `.nif`/`.dds`, changing the temp file does not
  reload what is already in memory. Create/copy it **before** the resource is loaded.
- **BSA**: `Copy` only looks inside BSAs from `kDataLoaded` on. Before that it only finds loose
  files.
- An **external** process (e.g. opening the temp file in Notepad++ while the game runs) may get
  "file in use" if it does not ask for `FILE_SHARE_DELETE`.
- Writing a **new** file inside a folder that only exists because of temp files (it does not
  exist in the real `Data`) fails: use `Create` for new files.
- Code that opens files directly through `NtCreateFile` (below Win32) is not redirected. The
  game, the CRT and `std::filesystem` all use Win32.

## Out-of-game test

`tests\run_tests.bat` builds the real `Store.cpp` + `Hooks.cpp` into a plain .exe (the .exe's
folder plays the part of the game folder) and checks 67 cases: redirection through
`ifstream`/`ofstream`, `std::filesystem`, Win32 A/W, folder listings, "save to .tmp + rename",
`copy_file`, `fs::remove`, deleting while the file is open, invalid paths and **CTD**. In the CTD
test a child process creates a temp file and dies by `TerminateProcess`; the test checks that
Windows deleted the file and that the next launch's sweep deletes the folder. It does not replace
testing in game: MO2 and BSAs only exist there.

## Build

CMake + vcpkg (`commonlibsse-ng`, `minhook`), `debug`/`release` presets, `x64-windows-static`
triplet. With `SKYRIM_MODS_FOLDER` set, the build copies the output to
`<mods>\TempFileFramework\SKSE\Plugins\`.
