[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $SourceDir,

    [string] $Repository = 'https://invent.kde.org/qt/qt/qtscript.git',

    [switch] $IncludePortedTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Run a native command with stderr merged into the success stream.
# Windows PowerShell 5.1 turns any native stderr line into a terminating
# error under $ErrorActionPreference = 'Stop' (plain 2>&1 does not help);
# pwsh 7 ignores native stderr entirely. This helper makes both behave the
# same and keeps $LASTEXITCODE as the single gatekeeper for success.
function Invoke-Native {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string] $FilePath,
        [Parameter(ValueFromRemainingArguments)][object[]] $Arguments
    )
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $FilePath @Arguments 2>&1
        $global:LASTEXITCODE = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $eap
    }
}

$baseCommit = 'bcd7cae6215df8f1c8b45a338f3327da51edeaff'
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

    # Merge stderr so healthy progress chatter cannot terminate the run:
    # Windows PowerShell 5.1 treats every native stderr line as an error
    # under $ErrorActionPreference = 'Stop'; pwsh 7 does not.
    New-Item -ItemType Directory -Path $SourceDir -Force | Out-Null
    $null = Invoke-Native git -C $SourceDir init
    if ($LASTEXITCODE -eq 0) { $null = Invoke-Native git -C $SourceDir remote add origin $Repository }
    if ($LASTEXITCODE -eq 0) { $null = Invoke-Native git -C $SourceDir fetch --depth 1 origin $baseCommit }
    if ($LASTEXITCODE -eq 0) { $null = Invoke-Native git -C $SourceDir checkout --detach FETCH_HEAD }
    if ($LASTEXITCODE -ne 0) {
        Write-Host $null
        throw "Unable to fetch QtScript base commit $baseCommit from $Repository"
    }
}

if (Test-Path -LiteralPath (Join-Path $SourceDir '.git\rebase-apply')) {
    throw "SourceDir has an interrupted git am session (.git/rebase-apply). Resolve or abort it first: $SourceDir"
}

$dirty = Invoke-Native git -C $SourceDir status --porcelain
if ($LASTEXITCODE -ne 0) {
    throw "SourceDir is not a Git work tree: $SourceDir"
}
if ($dirty) {
    throw "The QtScript source tree has uncommitted changes: $SourceDir"
}

function Apply-Patches {
    param(
        [Parameter(Mandatory)][string] $PatchDirectory,
        [Parameter(Mandatory)][string] $MarkerName,
        [string] $RequiredHead
    )

    $patches = @(Get-ChildItem -LiteralPath $PatchDirectory -Filter '*.patch' | Sort-Object Name)
    if ($patches.Count -eq 0) {
        throw "No patches were found in $PatchDirectory"
    }

    $fingerprint = ((& git hash-object -- $patches.FullName) -join "`n").Trim()
    if ($LASTEXITCODE -ne 0) { throw "Unable to fingerprint patches in $PatchDirectory" }
    $marker = Join-Path $SourceDir ".git\$MarkerName"
    if (Test-Path -LiteralPath $marker) {
        if ((Get-Content -LiteralPath $marker -Raw).Trim() -ne $fingerprint) {
            throw "The patch series changed after it was applied. Use a fresh SourceDir: $SourceDir"
        }
        return
    }
    if ($RequiredHead) {
        $head = (& git -C $SourceDir rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or $head -ne $RequiredHead) {
            throw "SourceDir is not at the pinned QtScript base $RequiredHead and has no patch marker: $SourceDir"
        }
    }

    Write-Host "Applying $($patches.Count) patches from $PatchDirectory"
    $amOutput = Invoke-Native git -C $SourceDir `
        -c 'user.name=QtScript Qt 6 patch set' `
        -c 'user.email=qtscript-qt6@local.invalid' `
        am $patches.FullName
    if ($LASTEXITCODE -ne 0) {
        $null = Invoke-Native git -C $SourceDir am --abort
        Write-Host $amOutput
        throw "Failed to apply patches from $PatchDirectory"
    }
    Set-Content -LiteralPath $marker -Value $fingerprint -NoNewline
}

Apply-Patches (Join-Path $repositoryRoot 'patches\quickjs') `
    'qtscript-quickjs-patches' $baseCommit

if ($IncludePortedTests) {
    Apply-Patches (Join-Path $repositoryRoot 'patches\optional\tests') `
        'qtscript-optional-test-patches'
}

Write-Host "Prepared QtScript source at $SourceDir"
