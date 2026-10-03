; SPDX-License-Identifier: MIT
; Copyright (c) 2026 HDharder, Temp File Framework, https://github.com/HDharder/Skyrim-Temp-File-Framework
; This script source is MIT licensed (like TempFileAPI.h) so any mod can compile against it.
Scriptname TempFile Hidden
{Temp File Framework: session-only overlay files for paths inside Data.
Paths are relative to Data ("SKSE/Plugins/MyMod/config.json"). Every temp file is deleted when
the game closes (even on CTD) and the original file is never modified.}

; Makes a temp copy of the current file at asPath, loose or from a BSA (BSAs only after the
; game data has loaded). If a temp file already exists it is reused as is. Returns the real path of the
; temp file, or "" if the original was not found.
string Function Copy(string asPath) global native

; Creates the temp file with this text content, or replaces the content of the existing one.
bool Function Create(string asPath, string asContent) global native

; Deletes the temp file. The original (if any) becomes visible again.
bool Function Delete(string asPath) global native

; True if a temp file currently exists for asPath.
bool Function Exists(string asPath) global native

; Absolute path of the temp file on disk, or "" if there is none.
string Function GetRealPath(string asPath) global native

; Reloads every texture the game has already loaded from this .dds path, from whatever the path
; holds now. Create, Copy and Delete of a .dds already do this by themselves; call it when the file
; changed some other way (another mod writing to the Data path, for example). The swap happens a
; few frames later. Returns how many loaded textures were queued (0: not loaded right now).
; Needs framework 1.2.0 or later.
int Function ReloadTexture(string asPath) global native
