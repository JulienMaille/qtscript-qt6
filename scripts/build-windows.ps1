[CmdletBinding()]
param(
    [string] $QtRoot = $env:QT_ROOT_DIR,

    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [string] $WorkRoot,

    [int] $Parallel = [Environment]::ProcessorCount,

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

if (-not $QtRoot) { throw 'Specify -QtRoot or set QT_ROOT_DIR.' }
$QtRoot = [System.IO.Path]::GetFullPath($QtRoot)
$qtKey = [BitConverter]::ToString(([Security.Cryptography.SHA256]::Create()).ComputeHash([Text.Encoding]::UTF8.GetBytes($QtRoot))).Replace('-', '').Substring(0, 12).ToLowerInvariant()
$qtCMake = Join-Path $QtRoot 'bin\qt-cmake-private.bat'
if (-not (Test-Path -LiteralPath $qtCMake)) {
    throw "Qt private module build helper was not found: $qtCMake"
}

# MSVC toolchain (runners and local shells do not have cl.exe on PATH).
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installPath = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    $vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) {
        throw 'MSVC toolchain not found via vswhere.'
    }
    foreach ($line in (cmd /c "call `"$vcvars`" >nul 2>&1 && set")) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

$repositoryRoot = Split-Path $PSScriptRoot -Parent
if (-not $WorkRoot) { $WorkRoot = Join-Path $repositoryRoot ".work\$qtKey\$Configuration" }
$sourceDir = Join-Path $WorkRoot 'src'
$buildDir = Join-Path $WorkRoot 'build'

$applyArgs = @{ SourceDir = $sourceDir }
if ($IncludePortedTests) { $applyArgs['IncludePortedTests'] = $true }
& (Join-Path $PSScriptRoot 'apply-patches.ps1') @applyArgs

$testsOption = if ($IncludePortedTests) { '-DQT_BUILD_TESTS=ON' } else { '-DQT_BUILD_TESTS=OFF' }
# qt-cmake-private forces RelWithDebInfo;Debug; map Release onto Qt's main
# configuration instead of fighting it with -DCMAKE_CONFIGURATION_TYPES.
$effectiveConfiguration = if ($Configuration -eq 'Debug') { 'Debug' } else { 'RelWithDebInfo' }
Invoke-Native $qtCMake -S $sourceDir -B $buildDir -G 'Ninja Multi-Config' `
    "-DCMAKE_INSTALL_PREFIX=$($QtRoot.Replace('\', '/'))" `
    $testsOption -DQT_BUILD_EXAMPLES=OFF
if ($LASTEXITCODE -ne 0) { throw 'QtScript configuration failed.' }

Invoke-Native cmake --build $buildDir --config $effectiveConfiguration --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw 'QtScript build failed.' }
Invoke-Native cmake --install $buildDir --config $effectiveConfiguration
if ($LASTEXITCODE -ne 0) { throw 'QtScript installation failed.' }

$dll = Join-Path $QtRoot "bin\$(if ($Configuration -eq 'Debug') { 'Qt6Scriptd.dll' } else { 'Qt6Script.dll' })"
if (-not (Test-Path -LiteralPath $dll)) { throw "Built DLL was not found: $dll" }
# The one claim worth asserting: the module does not link Core5Compat.
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    if (& $dumpbin.Source /DEPENDENTS $dll | Select-String 'Core5Compat|Qt5Compat') {
        throw 'QtScript links to Core5Compat or Qt5Compat.'
    }
}

# Out-of-tree smoke: build and run the external consumer against the install.
$smokeDir = Join-Path $WorkRoot 'smoke-build'
Invoke-Native cmake -S (Join-Path $repositoryRoot 'tests\smoke') -B $smokeDir `
    '-G', 'Ninja Multi-Config' `
    "-DCMAKE_CONFIGURATION_TYPES=$Configuration" `
    "-DCMAKE_PREFIX_PATH=$($QtRoot.Replace('\', '/'))"
if ($LASTEXITCODE -ne 0) { throw 'Smoke configuration failed.' }
Invoke-Native cmake --build $smokeDir --config $Configuration --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw 'Smoke build failed.' }
$env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"
Invoke-Native ctest --test-dir $smokeDir -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Smoke test failed.' }

Write-Host "Built and installed Qt6Script $Configuration into $QtRoot"
