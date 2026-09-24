# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 HDharder - Temp File Framework
# Builds the release archive from an existing Release build:
#   dist\TempFileFramework-<version>.zip   (install-ready: drop into MO2/Vortex)
#     SKSE\Plugins\TempFileFramework.dll
#     Scripts\TempFile.pex
#     Scripts\Source\TempFile.psc
#
# No PDB and no tester: a PDB is a map of the machine that built it. The DLL copy is scrubbed of
# source paths that prebuilt libraries (CommonLibSSE-NG from vcpkg) embed in their messages, and
# the script refuses to produce an archive if anything tied to the build machine is left.
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build\release'
$dll = Join-Path $build 'TempFileFramework.dll'
if (-not (Test-Path $dll)) { throw "Build the release preset first: '$dll' is missing" }

$dist = Join-Path $root 'dist'
$stage = Join-Path $dist "TempFileFramework-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'SKSE\Plugins'), (Join-Path $stage 'Scripts\Source') | Out-Null

# --- DLL: shorten every embedded path that starts with the project folder -------------------
$bytes = [IO.File]::ReadAllBytes($dll)
$prefix = [Text.Encoding]::ASCII.GetBytes((Resolve-Path $root).Path.TrimEnd('\') + '\')
$scrubbed = 0
for ($i = 0; $i -le $bytes.Length - $prefix.Length; $i++) {
    $match = $true
    for ($k = 0; $k -lt $prefix.Length; $k++) {
        $a = $bytes[$i + $k]; $b = $prefix[$k]
        if ($a -ge 65 -and $a -le 90) { $a += 32 }
        if ($b -ge 65 -and $b -le 90) { $b += 32 }
        if ($a -ne $b) { $match = $false; break }
    }
    if (-not $match) { continue }
    # A NUL-terminated string: drop the prefix, shift the rest left, pad the freed bytes with NULs.
    $end = $i + $prefix.Length
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
    $tail = $end - ($i + $prefix.Length)
    [Array]::Copy($bytes, $i + $prefix.Length, $bytes, $i, $tail)
    for ($z = $i + $tail; $z -lt $end; $z++) { $bytes[$z] = 0 }
    $scrubbed++
}
[IO.File]::WriteAllBytes((Join-Path $stage 'SKSE\Plugins\TempFileFramework.dll'), $bytes)
Write-Host "DLL: shortened $scrubbed embedded path(s)"

Copy-Item (Join-Path $root 'Scripts\TempFile.pex') (Join-Path $stage 'Scripts\TempFile.pex')
Copy-Item (Join-Path $root 'Scripts\Source\TempFile.psc') (Join-Path $stage 'Scripts\Source\TempFile.psc')

# Licenses travel with the binaries (GPL-3.0 for the framework, the notices of the statically
# linked libraries) in the plugin's own folder, so nothing lands loose in Data.
$docs = Join-Path $stage 'SKSE\Plugins\TempFileFramework'
New-Item -ItemType Directory -Force $docs | Out-Null
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $docs 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.txt') (Join-Path $docs 'THIRD_PARTY_NOTICES.txt')
@"
Temp File Framework $Version
Copyright (c) 2026 HDharder

Licensed under the GNU General Public License v3.0 (LICENSE.txt).
Source code: https://github.com/HDharder/Skyrim-Temp-File-Framework/tree/v$Version
The API header (TempFileAPI.h) and the Papyrus script source (TempFile.psc) are MIT licensed.
Third-party notices: THIRD_PARTY_NOTICES.txt
"@ | Set-Content (Join-Path $docs 'README.txt') -Encoding ASCII

# --- Refuse to ship anything tied to this machine ------------------------------------------
$markers = @((Resolve-Path $root).Path, $env:USERNAME, $env:COMPUTERNAME, $env:USERPROFILE) | Where-Object { $_ }
foreach ($file in Get-ChildItem $stage -Recurse -File) {
    $raw = [IO.File]::ReadAllBytes($file.FullName)
    $ascii = [Text.Encoding]::ASCII.GetString($raw)
    $wide = [Text.Encoding]::Unicode.GetString($raw)
    foreach ($marker in $markers) {
        if ($ascii.IndexOf($marker, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $wide.IndexOf($marker, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "'$($file.Name)' still contains a machine-specific string - not packaging"
        }
    }
}

# Compress-Archive (PowerShell 5) writes '\' inside the archive, which some mod tools misread;
# build it by hand with the standard '/' separator.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = Join-Path $dist "TempFileFramework-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $stage -Recurse -File) {
        $entry = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Write-Host "Wrote $zip"
