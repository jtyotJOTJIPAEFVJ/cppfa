# Builds cpp/PyfaReader.sln (Release x64) and copies PyfaReader.exe plus its data
# files into an installed pyfa, where service/moduleOptimizer.py looks for them.
#
# Usage (from anywhere):
#   powershell -ExecutionPolicy Bypass -File scripts\build_reader.ps1 (-PyfaInstall <pyfa dir> | -NoCopy)
# There is no default install: a dev build only goes over a pyfa that is named.

param(
    [string]$PyfaInstall,
    [switch]$NoCopy
)

$ErrorActionPreference = 'Stop'

if (-not $NoCopy) {
    if (-not $PyfaInstall) { throw 'Pass -PyfaInstall <pyfa dir> to copy into an installed pyfa, or -NoCopy to only build' }
    if (-not (Test-Path (Join-Path $PyfaInstall 'pyfa.exe'))) { throw "No pyfa.exe in $PyfaInstall" }
}

$repo = Split-Path -Parent $PSScriptRoot
$sln = Join-Path $repo 'cpp\PyfaReader.sln'
$outDir = Join-Path $repo 'cpp\bin\Release'
$files = 'PyfaReader.exe', 'effect_modifiers.tsv', 'warfare_buffs.tsv'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere; is Visual Studio installed?" }
$msbuild = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild.exe not found via vswhere' }

Write-Host "Building $sln with $msbuild"
& $msbuild $sln /m /nologo /v:minimal /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }

if ($NoCopy) { return }

$dest = Join-Path $PyfaInstall 'app\cpp\bin\Release'
New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($name in $files) {
    Copy-Item (Join-Path $outDir $name) $dest -Force
    Write-Host "Copied $name -> $dest"
}
