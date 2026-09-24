# Compiles Scripts/Source/TempFile.psc into Scripts/TempFile.pex with the Creation Kit's compiler.
# Only needed when the .psc changes - the compiled .pex is committed, so nobody else needs the CK.
#
# The game folder comes from SKYRIM_FOLDER, or the default Steam location. The compiler needs the
# game's TESV_Papyrus_Flags.flg, which ships inside Data\Scripts.zip: it is extracted into
# build\papyrus, never into the game folder.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path $PSScriptRoot -Parent
$game = if ($env:SKYRIM_FOLDER) { $env:SKYRIM_FOLDER } else { 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition' }
$compiler = Join-Path $game 'Papyrus Compiler\PapyrusCompiler.exe'
if (-not (Test-Path $compiler)) { throw "Papyrus compiler not found at '$compiler' (install the Creation Kit or set SKYRIM_FOLDER)" }

$work = Join-Path $root 'build\papyrus'
New-Item -ItemType Directory -Force $work | Out-Null
$flags = Join-Path $work 'TESV_Papyrus_Flags.flg'
if (-not (Test-Path $flags)) {
    $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $game 'Data\Scripts.zip'))
    try {
        $entry = $zip.Entries | Where-Object { $_.Name -eq 'TESV_Papyrus_Flags.flg' } | Select-Object -First 1
        if (-not $entry) { throw 'TESV_Papyrus_Flags.flg not found in Data\Scripts.zip' }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $flags, $true)
    } finally {
        $zip.Dispose()
    }
}

$source = Join-Path $root 'Scripts\Source'
& $compiler (Join-Path $source 'TempFile.psc') '-f=TESV_Papyrus_Flags.flg' "-i=$source;$work" "-o=$(Join-Path $root 'Scripts')"
if ($LASTEXITCODE -ne 0) { throw "Papyrus compilation failed ($LASTEXITCODE)" }

# The compiler writes the Windows user name and the computer name into the .pex header (debug
# metadata the game never reads). Blank them so the committed file carries nothing about the
# machine that built it. Header: magic(4) version(2+2) timestamp(8), then three big-endian
# length-prefixed strings: source file name, user name, computer name.
$pex = Join-Path $root 'Scripts\TempFile.pex'
$bytes = [IO.File]::ReadAllBytes($pex)
function Read-Length([byte[]]$data, [int]$at) { ($data[$at] -shl 8) -bor $data[$at + 1] }
$pos = 16
$sourceLength = Read-Length $bytes $pos
$afterSource = $pos + 2 + $sourceLength
$userLength = Read-Length $bytes $afterSource
$machineAt = $afterSource + 2 + $userLength
$machineLength = Read-Length $bytes $machineAt
$rest = $machineAt + 2 + $machineLength
$clean = [byte[]]($bytes[0..($afterSource - 1)] + @(0, 0, 0, 0) + $bytes[$rest..($bytes.Length - 1)])
[IO.File]::WriteAllBytes($pex, $clean)
Write-Host "Blanked the user/computer name in $pex"
