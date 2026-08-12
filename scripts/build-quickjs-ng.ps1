[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'All')]
    [string] $Configuration = 'Release',

    [string] $WorkRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.work\quickjs-ng'),

    [string] $QuickJsSource = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\quickjs-ng'),

    [int] $Parallel = [Environment]::ProcessorCount,

    [string] $Generator = 'Ninja',

    [string] $Architecture = 'x64',

    [string] $Toolset = 'host=x64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedCommit = '954dc53628e36891f93c359aa60895c2ae3dac6b'
$repositoryRoot = Split-Path $PSScriptRoot -Parent

function Get-VsDevCmdBat {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installationPath = & $vswhere -latest -products '*' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
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
        throw 'MSVC toolchain not found: cl.exe is not on PATH and no vcvars64.bat was found.'
    }

    Write-Host "Importing MSVC environment from $vcvars"
    $output = cmd /c "call `"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to import the MSVC environment from $vcvars"
    }

    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'The MSVC environment was imported but cl.exe is still not on PATH.'
    }
}

function Invoke-CMake {
    param([Parameter(Mandatory)][string[]] $Arguments)

    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake failed with exit code $LASTEXITCODE"
    }
}

if ($Parallel -lt 1) {
    throw 'Parallel must be a positive integer.'
}

Import-VsDevEnvironment

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw 'cmake.exe was not found on PATH.'
}
if (($Generator -like 'Ninja*') -and
    -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    throw 'ninja.exe was not found on PATH for the selected generator.'
}
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
    throw 'git.exe was not found on PATH.'
}

$QuickJsSource = [System.IO.Path]::GetFullPath($QuickJsSource)
$WorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
if (-not (Test-Path -LiteralPath (Join-Path $QuickJsSource 'CMakeLists.txt')) -or
    -not (Test-Path -LiteralPath (Join-Path $QuickJsSource 'quickjs.h'))) {
    throw "QuickJS-NG submodule is not initialized: $QuickJsSource`nRun 'git submodule update --init --recursive' first."
}

$actualCommit = (& git -C $QuickJsSource rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $expectedCommit) {
    throw "QuickJS-NG revision mismatch: expected $expectedCommit, got $actualCommit."
}

$header = Get-Content -LiteralPath (Join-Path $QuickJsSource 'quickjs.h') -Raw
foreach ($versionLine in @(
    '#define QJS_VERSION_MAJOR 0',
    '#define QJS_VERSION_MINOR 16',
    '#define QJS_VERSION_PATCH 1'
)) {
    if ($header -notmatch [regex]::Escape($versionLine)) {
        throw "QuickJS-NG source is not version 0.16.1: $QuickJsSource\quickjs.h"
    }
}

$configurations = switch ($Configuration) {
    'Debug' { @('Debug') }
    'Release' { @('Release') }
    'All' { @('Debug', 'Release') }
}

$generatorArgs = @('-G', $Generator)
if ($Generator -like 'Visual Studio*') {
    if ($Architecture) { $generatorArgs += @('-A', $Architecture) }
    if ($Toolset) { $generatorArgs += @('-T', $Toolset) }
}
$isMultiConfig = $Generator -like '*Multi-Config' -or $Generator -like 'Visual Studio*'

foreach ($buildConfiguration in $configurations) {
    $configurationRoot = Join-Path $WorkRoot $buildConfiguration
    $buildDir = Join-Path $configurationRoot 'build'
    $installDir = Join-Path $configurationRoot 'install'
    New-Item -ItemType Directory -Path $configurationRoot, $installDir -Force | Out-Null

    $configureArgs = @(
        '-S', $QuickJsSource,
        '-B', $buildDir
    ) + $generatorArgs + @(
        "-DCMAKE_INSTALL_PREFIX=$installDir",
        '-DBUILD_SHARED_LIBS=OFF',
        '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
        '-DQJS_BUILD_LIBC=OFF',
        '-DQJS_BUILD_EXAMPLES=OFF',
        '-DQJS_ENABLE_INSTALL=OFF',
        '-DQJS_BUILD_WERROR=OFF'
    )

    if ($isMultiConfig) {
        $configureArgs += "-DCMAKE_CONFIGURATION_TYPES=$buildConfiguration"
    } else {
        $configureArgs += "-DCMAKE_BUILD_TYPE=$buildConfiguration"
    }

    if ($env:VCToolsInstallDir) {
        $runtime = if ($buildConfiguration -eq 'Debug') {
            'MultiThreadedDebugDLL'
        } else {
            'MultiThreadedDLL'
        }
        $configureArgs += "-DCMAKE_MSVC_RUNTIME_LIBRARY=$runtime"
    }

    Write-Host "Configuring QuickJS-NG 0.16.1 $buildConfiguration"
    Invoke-CMake $configureArgs

    $buildArgs = @('--build', $buildDir, '--target', 'qjs', 'qjs_exe', 'api-test', '--parallel', $Parallel)
    if ($isMultiConfig) { $buildArgs += @('--config', $buildConfiguration) }
    Invoke-CMake $buildArgs

    $artifactRoot = $buildDir
    if ($isMultiConfig) { $artifactRoot = Join-Path $buildDir $buildConfiguration }
    $qjsLibrary = Join-Path $artifactRoot 'qjs.lib'
    $qjsExecutable = Join-Path $artifactRoot 'qjs.exe'
    $apiTest = Join-Path $artifactRoot 'api-test.exe'

    foreach ($requiredArtifact in @($qjsLibrary, $qjsExecutable, $apiTest)) {
        if (-not (Test-Path -LiteralPath $requiredArtifact)) {
            throw "QuickJS-NG artifact was not produced: $requiredArtifact"
        }
    }

    $sharedEngineArtifacts = @(Get-ChildItem -LiteralPath $buildDir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^(qjs|libqjs)\.(dll|so|dylib)(\..*)?$' })
    if ($sharedEngineArtifacts.Count -ne 0) {
        throw "QuickJS-NG produced a shared engine library: $($sharedEngineArtifacts.FullName -join ', ')"
    }

    Write-Host "Running QuickJS-NG api-test ($buildConfiguration)"
    & $apiTest
    if ($LASTEXITCODE -ne 0) { throw "QuickJS-NG api-test failed for $buildConfiguration" }

    Write-Host "Running QuickJS-NG expression smoke ($buildConfiguration)"
    $expression = "if (1 + 2 !== 3 || typeof BigInt !== 'function') throw new Error('QuickJS-NG expression smoke failed'); console.log('QuickJS-NG expression smoke passed')"
    & $qjsExecutable -e $expression
    if ($LASTEXITCODE -ne 0) { throw "QuickJS-NG expression smoke failed for $buildConfiguration" }
}

Write-Host "QuickJS-NG 0.16.1 static build passed: $($configurations -join ', ')"
