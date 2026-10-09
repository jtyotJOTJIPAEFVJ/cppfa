# Regenerates the C++ optimizer's game data after updating pyfa (cpp/FIT_ENGINE.md, section 2):
#   1. eve.db from staticdata (db_update.py)
#   2. cpp/PyfaReader/data/effect_modifiers.tsv + warfare_buffs.tsv from eos/effects.py
#      and eos/saveddata/fit.py (gen_effect_modifiers.py)
#   3. cpp/PyfaReader/src/generated/FitSchema.generated.h from eve.db + the TSV (gen_fit_schema.py)
#   4. builds PyfaReader.exe and copies it with its data into an installed pyfa
# Then validate (FIT_ENGINE.md, section 6), e.g. PyfaReader --search-check.
#
# Usage (from anywhere):
#   powershell -ExecutionPolicy Bypass -File scripts\update_cpp_data.ps1 [-PyfaInstall I:\pelit\pyfa] [-SkipEveDb] [-NoCopy]
#
# Uses the same Windows environment as build_pyfa.ps1 (%LOCALAPPDATA%\pyfa-build\venv).

param(
    [string]$PyfaInstall = 'I:\pelit\pyfa',
    [switch]$SkipEveDb,
    [switch]$NoCopy
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

function Invoke-Step([string]$what, [scriptblock]$cmd) {
    Write-Host "== $what"
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

if (-not (Get-Command uv -ErrorAction SilentlyContinue)) { throw 'uv not found on PATH' }
$env:UV_PROJECT_ENVIRONMENT = Join-Path $env:LOCALAPPDATA 'pyfa-build\venv'
# Same groups as build_pyfa.ps1, so the two don't re-sync each other's packages away.
Invoke-Step 'Sync Windows environment' { uv sync --frozen --no-default-groups --group packaging --group wx-binary }

if (-not $SkipEveDb) {
    Invoke-Step 'Build eve.db' { uv run --no-sync python db_update.py }
}
Invoke-Step 'Generate effect_modifiers.tsv, warfare_buffs.tsv' { uv run --no-sync python cpp/tools/gen_effect_modifiers.py }
Invoke-Step 'Generate FitSchema.generated.h' { uv run --no-sync python cpp/tools/gen_fit_schema.py }

$reader = @{}
if ($NoCopy) { $reader.NoCopy = $true } else { $reader.PyfaInstall = $PyfaInstall }
Invoke-Step 'Build PyfaReader' { & (Join-Path $PSScriptRoot 'build_reader.ps1') @reader }

Write-Host '== Changed generated files'
git status --short -- eve.db cpp/PyfaReader/data cpp/PyfaReader/src/generated
