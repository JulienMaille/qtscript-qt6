[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $SourceDir,

    [string] $Repository = 'https://invent.kde.org/qt/qt/qtscript.git',

    [switch] $IncludePortedTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$baseBranch = '5.15.19'
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$SourceDir = [System.IO.Path]::GetFullPath($SourceDir)

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'git was not found on PATH.'
}

if (-not (Test-Path -LiteralPath (Join-Path $SourceDir '.git'))) {
    if (Test-Path -LiteralPath $SourceDir) {
        $existing = @(Get-ChildItem -LiteralPath $SourceDir -Force)
        if ($existing.Count -ne 0) {
            throw "SourceDir exists and is not empty: $SourceDir"
        }
    } else {
        New-Item -ItemType Directory -Path (Split-Path $SourceDir -Parent) -Force | Out-Null
    }

    & git clone --depth 1 --branch $baseBranch --single-branch $Repository $SourceDir
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to clone KDE QtScript from $Repository"
    }
}

if (Test-Path -LiteralPath (Join-Path $SourceDir '.git\rebase-apply')) {
    throw "SourceDir has an interrupted git am session (.git/rebase-apply). Resolve or abort it first: $SourceDir"
}

$dirty = (& git -C $SourceDir status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw "SourceDir is not a Git work tree: $SourceDir"
}
if ($dirty) {
    throw "The QtScript source tree has uncommitted changes: $SourceDir"
}

function Apply-Patches {
    param([Parameter(Mandatory)][string] $PatchDirectory)

    $patches = @(Get-ChildItem -LiteralPath $PatchDirectory -Filter '*.patch' | Sort-Object Name)
    if ($patches.Count -eq 0) {
        throw "No patches were found in $PatchDirectory"
    }

    Write-Host "Applying $($patches.Count) patches from $PatchDirectory"
    & git -C $SourceDir `
        -c 'user.name=QtScript Qt 6 patch set' `
        -c 'user.email=qtscript-qt6@local.invalid' `
        am $patches.FullName
    if ($LASTEXITCODE -ne 0) {
        & git -C $SourceDir am --abort
        throw "Failed to apply patches from $PatchDirectory"
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $SourceDir 'CMakeLists.txt'))) {
    Apply-Patches (Join-Path $repositoryRoot 'patches')
}

if ($IncludePortedTests -and
    -not (Test-Path -LiteralPath (Join-Path $SourceDir 'tests\CMakeLists.txt'))) {
    Apply-Patches (Join-Path $repositoryRoot 'patches\optional\tests')
}

Write-Host "Prepared QtScript source at $SourceDir"
