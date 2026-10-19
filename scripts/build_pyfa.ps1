# Builds the Windows pyfa (PyInstaller, as in .appveyor.yml) with the module optimizer
# bundled, and copies it over an installed pyfa.
#
# Usage (from anywhere):
#   powershell -ExecutionPolicy Bypass -File scripts\build_pyfa.ps1 (-PyfaInstall <pyfa dir> | -NoCopy) [-Zip]
# There is no default install: a dev build only goes over a pyfa that is named.
# -Zip also packs dist\pyfa into dist\pyfa-<version>-cppfa-win64.zip (a portable pyfa, for a
# GitHub release), version from version.yml.
#
# The repo's .venv is the WSL (Linux) one, so the Windows environment lives in
# %LOCALAPPDATA%\pyfa-build\venv (uv, Python from .python-version).

param(
    [string]$PyfaInstall,
    [switch]$NoCopy,
    [switch]$Zip
)

$ErrorActionPreference = 'Stop'

if (-not $NoCopy) {
    if (-not $PyfaInstall) { throw 'Pass -PyfaInstall <pyfa dir> to copy into an installed pyfa, or -NoCopy to only build' }
    if (-not (Test-Path (Join-Path $PyfaInstall 'pyfa.exe'))) { throw "No pyfa.exe in $PyfaInstall" }
}

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

function Invoke-Step([string]$what, [scriptblock]$cmd) {
    Write-Host "== $what"
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

if (-not (Get-Command uv -ErrorAction SilentlyContinue)) { throw 'uv not found on PATH' }
$env:UV_PROJECT_ENVIRONMENT = Join-Path $env:LOCALAPPDATA 'pyfa-build\venv'

# The optimizer exe first: pyfa.spec bundles cpp\bin\Release\PyfaReader.exe when it exists.
Invoke-Step 'Build PyfaReader' { & (Join-Path $PSScriptRoot 'build_reader.ps1') -NoCopy }

Invoke-Step 'Sync Windows environment' { uv sync --frozen --no-default-groups --group packaging --group wx-binary }
Invoke-Step 'Compile language files' { uv run --no-sync python scripts/compile_lang.py }
if (-not (Test-Path 'eve.db')) {
    Invoke-Step 'Build eve.db' { uv run --no-sync python db_update.py }
}
Invoke-Step 'PyInstaller' { uv run --no-sync python -m PyInstaller --clean -y pyfa.spec }
# See pyfa-org/pyfa#1622
Copy-Item 'dist_assets\win\pyfa.exe.manifest' 'dist\pyfa\' -Force

if ($Zip) {
    $version = (Select-String -Path 'version.yml' -Pattern '^version:\s*(\S+)').Matches[0].Groups[1].Value
    $zipPath = Join-Path $repo "dist\pyfa-$version-cppfa-win64.zip"
    Write-Host "== Zip dist\pyfa -> $zipPath"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    # .NET's ZipFile rather than Compress-Archive: Windows PowerShell's Compress-Archive writes
    # backslashes into entry names, which other unzip tools take as part of the file name.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $repo 'dist\pyfa'), $zipPath,
        [System.IO.Compression.CompressionLevel]::Optimal, $true)
}

if ($NoCopy) { return }

if (Get-Process pyfa -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$PyfaInstall\*" }) {
    throw "pyfa is running from $PyfaInstall; close it first"
}
# Adds and overwrites only: the installer's files (uninstaller etc.) stay.
Write-Host "== Copy dist\pyfa -> $PyfaInstall"
robocopy 'dist\pyfa' $PyfaInstall /E /NFL /NDL /NJH /NP /R:2 /W:2
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE" }
$global:LASTEXITCODE = 0
Write-Host "Copied pyfa to $PyfaInstall"
