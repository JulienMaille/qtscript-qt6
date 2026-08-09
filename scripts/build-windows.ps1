[CmdletBinding()]
param(
    [string] $QtRoot = $env:QT_ROOT_DIR,

    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [string] $WorkRoot,

    [int] $Parallel = [Environment]::ProcessorCount,

    [string] $Generator,

    [string] $Architecture = 'x64',

    [string] $Toolset = 'host=x64',

    [switch] $IncludePortedTests,

    [switch] $UseQuickJS
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-VsDevCmdBat {
    # Prefer vswhere when present (GitHub runners); fall back to probing
    # the known VS roots so local installs (e.g. VS 2026 Community)
    # without vswhere still work.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installationPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $candidate = Join-Path $installationPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    foreach ($root in @(
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\18\Enterprise',
        'C:\Program Files\Microsoft Visual Studio\18\Professional',
        'C:\Program Files\Microsoft Visual Studio\18\Community'
    )) {
        $candidate = Join-Path $root 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Import-VsDevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vcvars = Get-VsDevCmdBat
    if (-not $vcvars) {
        throw 'MSVC toolchain not found: cl.exe is not on PATH and no vcvars64.bat was found via vswhere or the known VS roots.'
    }
    Write-Host "Importing MSVC environment from $vcvars"
    $output = cmd /c "call `"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "Failed to import the MSVC environment from $vcvars" }
    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'The MSVC environment was imported but cl.exe is still not on PATH.'
    }
}

$repositoryRoot = Split-Path $PSScriptRoot -Parent

if (-not $QtRoot) {
    throw 'Specify -QtRoot or set QT_ROOT_DIR.'
}
$QtRoot = [System.IO.Path]::GetFullPath($QtRoot)
$qtCMake = Join-Path $QtRoot 'bin\qt-cmake-private.bat'
if (-not (Test-Path -LiteralPath $qtCMake)) {
    throw "Qt private module build helper was not found: $qtCMake"
}
$qtPaths = @('qtpaths6.exe', 'qtpaths.exe') |
    ForEach-Object { Join-Path $QtRoot "bin\$_" } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if (-not $qtPaths) {
    throw "qtpaths was not found under $QtRoot\bin."
}
$qtVersion = (& $qtPaths --qt-version).Trim()
if ($LASTEXITCODE -ne 0 -or $qtVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "Unable to determine the Qt version from $qtPaths."
}
if ($Parallel -lt 1) {
    throw 'Parallel must be a positive integer.'
}
if (-not $WorkRoot) {
    $WorkRoot = Join-Path $repositoryRoot ".work\$qtVersion\$Configuration"
}
$WorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
Write-Host "Building QtScript package version $qtVersion for Qt at $QtRoot"
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw 'cmake.exe was not found on PATH.'
}
Import-VsDevEnvironment
if (($Generator -like 'Ninja*' -or -not $Generator) -and
    -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    throw 'ninja.exe was not found on PATH (required by the selected/default Ninja generator).'
}

$sourceDir = Join-Path $workRoot 'src'
$buildDir = Join-Path $workRoot 'build'
$smokeBuildDir = Join-Path $workRoot 'smoke-build'

$applyArgs = @{ SourceDir = $sourceDir }
if ($IncludePortedTests) { $applyArgs['IncludePortedTests'] = $true }
& (Join-Path $PSScriptRoot 'apply-patches.ps1') @applyArgs

if ($UseQuickJS) {
    $quickJsSource = Join-Path $repositoryRoot 'quickjs_migration\3rdparty\quickjs'
    if (-not (Test-Path -LiteralPath (Join-Path $quickJsSource 'quickjs.c'))) {
        throw "QuickJS sources are not initialized. Run 'git submodule update --init --recursive'."
    }
    Write-Host 'Applying QuickJS migration overrides...'
    $scriptDir = Join-Path $sourceDir 'src\script'
    $quickJsDestination = Join-Path $sourceDir 'src\3rdparty\quickjs'
    New-Item -ItemType Directory -Path $quickJsDestination -Force | Out-Null
    Copy-Item -Path (Join-Path $quickJsSource '*') -Destination $quickJsDestination -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\qscriptengine.h') -Destination (Join-Path $scriptDir 'api') -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\qscriptengine.cpp') -Destination (Join-Path $scriptDir 'api') -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\qscriptvalue.h') -Destination (Join-Path $scriptDir 'api') -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\qscriptvalue_p.h') -Destination (Join-Path $scriptDir 'api') -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\qscriptvalue.cpp') -Destination (Join-Path $scriptDir 'api') -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\qregexp.h') -Destination (Join-Path $scriptDir 'api') -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\qregexp.cpp') -Destination (Join-Path $scriptDir 'api') -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\qobject_bridge.cpp') -Destination (Join-Path $scriptDir 'bridge') -Force
    Copy-Item -Path (Join-Path $repositoryRoot 'quickjs_migration\CMakeLists.txt') -Destination $scriptDir -Force
}

# No -Generator: qt-cmake-private's built-in default (Ninja Multi-Config) is
# used. -A/-T only apply to Visual Studio generators; Ninja rejects them.
$generatorArgs = @()
if ($Generator) {
    $generatorArgs += @('-G', $Generator)
}
if ($Generator -like 'Visual Studio*') {
    if ($Architecture) { $generatorArgs += @('-A', $Architecture) }
    if ($Toolset) { $generatorArgs += @('-T', $Toolset) }
}

$installRoot = $QtRoot.Replace('\', '/')
$testsOption = if ($IncludePortedTests) { '-DQT_BUILD_TESTS=ON' } else { '-DQT_BUILD_TESTS=OFF' }
$configureArgs = @(
    '-S', $sourceDir,
    '-B', $buildDir,
    "-DCMAKE_INSTALL_PREFIX=$installRoot",
    "-DQT_REPO_MODULE_VERSION=$qtVersion",
    $testsOption,
    '-DQT_BUILD_EXAMPLES=OFF'
)
$configureArgs += $generatorArgs
& $qtCMake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'QtScript configuration failed.' }

$configurationLine = Get-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') |
    Where-Object { $_ -match '^CMAKE_CONFIGURATION_TYPES:[^=]+=' } |
    Select-Object -First 1
$isMultiConfig = [bool]$configurationLine
$effectiveConfiguration = $Configuration
if ($isMultiConfig) {
    $availableConfigurations = @($configurationLine.Split('=', 2)[1].Split(';'))
    if ($Configuration -eq 'Release' -and
        $availableConfigurations -notcontains 'Release' -and
        $availableConfigurations -contains 'RelWithDebInfo') {
        $effectiveConfiguration = 'RelWithDebInfo'
    }
    if ($availableConfigurations -notcontains $effectiveConfiguration) {
        throw "Configuration $Configuration is unavailable. Generated: $($availableConfigurations -join ', ')"
    }
    Write-Host "Using CMake configuration $effectiveConfiguration for requested $Configuration build"
} else {
    Write-Host "Using single-config CMake build type $effectiveConfiguration"
}

$buildArgs = @('--build', $buildDir)
if ($isMultiConfig) { $buildArgs += @('--config', $effectiveConfiguration) }
$buildArgs += @('--parallel', $Parallel)
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw 'QtScript build failed.' }

$installArgs = @('--install', $buildDir)
if ($isMultiConfig) { $installArgs += @('--config', $effectiveConfiguration) }
& cmake @installArgs
if ($LASTEXITCODE -ne 0) { throw 'QtScript installation failed.' }

if ($IncludePortedTests -and $Configuration -eq 'Debug') {
    Write-Host "Running ported upstream test suites (ctest, $effectiveConfiguration)..."
    $testArgs = @('--test-dir', $buildDir)
    if ($isMultiConfig) { $testArgs += @('-C', $effectiveConfiguration) }
    $testArgs += @('--parallel', $Parallel, '--output-on-failure')
    & ctest @testArgs
    if ($LASTEXITCODE -ne 0) { throw 'QtScript ported test suites failed.' }
}

$metadataFiles = @()
foreach ($root in @(
    (Join-Path $QtRoot 'lib\cmake\Qt6Script'),
    (Join-Path $QtRoot 'mkspecs\modules\qt_lib_script.pri')
)) {
    if (Test-Path -LiteralPath $root) {
        $metadataFiles += @(Get-ChildItem -LiteralPath $root -Recurse -File |
            Where-Object { $_.Extension -in '.cmake', '.pri', '.prl' })
    }
}
$forbiddenPaths = @(
    $sourceDir,
    $sourceDir.Replace('\', '/'),
    $buildDir,
    $buildDir.Replace('\', '/')
) | Select-Object -Unique
$metadataLeaks = @()
if ($metadataFiles.Count -ne 0) {
    $metadataLeaks = @(Select-String `
        -LiteralPath $metadataFiles.FullName `
        -SimpleMatch `
        -Pattern $forbiddenPaths)
}
if ($metadataLeaks.Count -ne 0) {
    $locations = $metadataLeaks |
        ForEach-Object { "$($_.Path):$($_.LineNumber)" } |
        Sort-Object -Unique
    throw "Installed metadata contains source/build paths:`n$($locations -join "`n")"
}
if ($metadataFiles.Count -eq 0) {
    throw "Installed QtScript metadata was not found under $QtRoot."
}
$compatibilityLeaks = @(Select-String `
    -LiteralPath $metadataFiles.FullName `
    -Pattern 'Core5Compat|Qt5Compat')
if ($compatibilityLeaks.Count -ne 0) {
    $locations = $compatibilityLeaks |
        ForEach-Object { "$($_.Path):$($_.LineNumber)" } |
        Sort-Object -Unique
    throw "Installed metadata contains a Core5Compat or Qt5Compat dependency:`n$($locations -join "`n")"
}

$scriptLibrary = Join-Path $QtRoot 'bin\Qt6Script.dll'
if (-not (Test-Path -LiteralPath $scriptLibrary)) {
    throw "Installed QtScript DLL was not found: $scriptLibrary"
}
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    throw 'dumpbin.exe was not found after importing the MSVC environment.'
}
$dependencies = & $dumpbin.Source /DEPENDENTS $scriptLibrary 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "Unable to inspect QtScript DLL dependencies: $scriptLibrary"
}
if ($dependencies -match 'Core5Compat|Qt5Compat') {
    throw 'QtScript links to Core5Compat or Qt5Compat.'
}

$smokeArgs = @(
    '-S', (Join-Path $repositoryRoot 'tests\smoke'),
    '-B', $smokeBuildDir,
    "-DCMAKE_PREFIX_PATH=$installRoot"
)
if ($isMultiConfig) {
    $smokeArgs += "-DCMAKE_CONFIGURATION_TYPES=$effectiveConfiguration"
} else {
    $smokeArgs += "-DCMAKE_BUILD_TYPE=$effectiveConfiguration"
}
$smokeArgs += $generatorArgs
& cmake @smokeArgs
if ($LASTEXITCODE -ne 0) { throw 'Smoke-test configuration failed.' }

$smokeBuildArgs = @('--build', $smokeBuildDir)
if ($isMultiConfig) { $smokeBuildArgs += @('--config', $effectiveConfiguration) }
$smokeBuildArgs += @('--parallel', $Parallel)
& cmake @smokeBuildArgs
if ($LASTEXITCODE -ne 0) { throw 'Smoke-test build failed.' }

$originalPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $QtRoot 'bin');$originalPath"
    $smokeTestArgs = @('--test-dir', $smokeBuildDir)
    if ($isMultiConfig) { $smokeTestArgs += @('-C', $effectiveConfiguration) }
    $smokeTestArgs += '--output-on-failure'
    & ctest @smokeTestArgs
    if ($LASTEXITCODE -ne 0) { throw 'Smoke test failed.' }
} finally {
    $env:PATH = $originalPath
}

Write-Host "QtScript $Configuration installed into $QtRoot"
