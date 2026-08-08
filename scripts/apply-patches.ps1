[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $SourceDir,

    [string] $Repository = 'https://invent.kde.org/qt/qt/qtscript.git',

    [switch] $IncludePortedTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$baseRevision = 'bcd7cae6215df8f1c8b45a338f3327da51edeaff'
$stages = @(
    [pscustomobject]@{
        Name = 'Qt 5.15.19 baseline'
        Tree = '9f515614bafcf1b8bf6741e77e0cded7ebe6b5f5'
        PatchDirectory = $null
    },
    [pscustomobject]@{
        Name = 'minimal Qt 6 core port'
        Tree = '730db66c641b124db4a21a8dc1b0883f6d99437f'
        PatchDirectory = 'patches'
    },
    [pscustomobject]@{
        Name = 'ported compatibility tests'
        Tree = '416ddcf14e38460bd94edc322cfaa1fae71c4da2'
        PatchDirectory = 'patches/optional/tests'
    }
)
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

    & git clone --no-checkout $Repository $SourceDir
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to clone KDE QtScript from $Repository"
    }
    & git -C $SourceDir checkout --detach $baseRevision
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to check out the pinned baseline $baseRevision"
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

$tree = (& git -C $SourceDir rev-parse 'HEAD^{tree}').Trim()
$currentStage = -1
for ($index = 0; $index -lt $stages.Count; ++$index) {
    if ($tree -eq $stages[$index].Tree) {
        $currentStage = $index
        break
    }
}
if ($currentStage -lt 0) {
    throw "Unexpected QtScript source tree $tree. Use a clean pinned baseline or a tree prepared by this script."
}

$targetStage = 1
if ($IncludePortedTests) { $targetStage = 2 }

if ($currentStage -gt $targetStage) {
    throw "The source already includes $($stages[$currentStage].Name), which exceeds the requested $($stages[$targetStage].Name)."
}
if ($currentStage -eq $targetStage) {
    Write-Host "QtScript is already prepared at tree $tree ($($stages[$targetStage].Name))"
    return
}

for ($stageIndex = $currentStage + 1; $stageIndex -le $targetStage; ++$stageIndex) {
    $stage = $stages[$stageIndex]
    $patchDir = Join-Path $repositoryRoot $stage.PatchDirectory
    $patches = @(Get-ChildItem -LiteralPath $patchDir -Filter '*.patch' | Sort-Object Name)
    if ($patches.Count -eq 0) {
        throw "No patches were found for $($stage.Name) in $patchDir"
    }

    foreach ($patch in $patches) {
        Write-Host "Applying $($patch.Name)"
        & git -C $SourceDir `
            -c 'user.name=QtScript Qt 6 patch set' `
            -c 'user.email=qtscript-qt6@local.invalid' `
            am $patch.FullName
        if ($LASTEXITCODE -ne 0) {
            & git -C $SourceDir am --abort
            throw "Failed to apply $($patch.Name)"
        }
    }

    $tree = (& git -C $SourceDir rev-parse 'HEAD^{tree}').Trim()
    if ($tree -ne $stage.Tree) {
        throw "Patch verification failed after $($stage.Name): tree=$tree, expected=$($stage.Tree)"
    }
}

Write-Host "Prepared QtScript at $SourceDir ($($stages[$targetStage].Name), tree $tree)"
