[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $BuildDir,

    [Parameter(Mandatory)]
    [string] $QtBin,

    [Parameter(Mandatory)]
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string] $Configuration,

    [Parameter(Mandatory)]
    [ValidateSet('qscriptcontext', 'qscriptcontextinfo', 'qscriptengine', 'qscriptvalue',
                 'qscriptextqobject', 'qscriptjstestsuite', 'qscriptv8testsuite')]
    [string] $Suite,

    [string] $Function,

    [string] $OutputDirectory = '',

    [int] $TimeoutSeconds = 120,

    [switch] $IsolateFunctions,

    [switch] $AllowTestFailures
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$QtBin = [System.IO.Path]::GetFullPath($QtBin)
if ($TimeoutSeconds -lt 1) {
    throw 'TimeoutSeconds must be positive.'
}

$scriptDirectory = Split-Path -Parent $PSCommandPath
$repositoryRoot = Split-Path -Parent $scriptDirectory
$suiteDirectory = Join-Path $BuildDir "tests\auto\$Suite\$Configuration"
$testExecutable = Join-Path $suiteDirectory "tst_$Suite.exe"
$quickJsBin = Join-Path $BuildDir 'bin'
$quickJsDllName = if ($Configuration -eq 'Debug') { 'Qt6Scriptd.dll' } else { 'Qt6Script.dll' }
$quickJsScript = Join-Path $quickJsBin $quickJsDllName
if (-not (Test-Path -LiteralPath $testExecutable)) {
    throw "Inherited test executable was not found: $testExecutable"
}
if (-not (Test-Path -LiteralPath $quickJsScript)) {
    throw "The just-built QtScript DLL was not found: $quickJsScript"
}
if (-not (Test-Path -LiteralPath (Join-Path $QtBin 'Qt6Core.dll'))) {
    throw "Qt runtime directory does not contain Qt6Core.dll: $QtBin"
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot '.work\inherited-tests'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$name = "$Suite-$Configuration-$stamp"
if ($Function) {
    $name += '-' + ($Function -replace '[^A-Za-z0-9_.-]', '_')
}
$stdoutPath = Join-Path $OutputDirectory "$name.stdout.txt"
$stderrPath = Join-Path $OutputDirectory "$name.stderr.txt"
$consolePath = Join-Path $OutputDirectory "$name.console.txt"
$metadataPath = Join-Path $OutputDirectory "$name.meta.txt"

# Qt's generated CTest wrappers can put an installed Qt6Script before the
# build output.  The build bin directory is deliberately first here; the
# executable directory is second because it may contain test-local Qt DLLs.
$pathParts = @($quickJsBin, $suiteDirectory, $QtBin) + @($env:PATH -split ';' | Where-Object { $_ })
$childPath = ($pathParts -join ';')
$qtPluginPath = Join-Path $BuildDir 'plugins'
$arguments = @()
if ($Function) {
    $arguments += $Function
}
$arguments += @('-o', "$stdoutPath,txt")

$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $testExecutable
$psi.WorkingDirectory = (Join-Path $BuildDir "tests\auto\$Suite")
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.Arguments = (($arguments | ForEach-Object {
    '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}) -join ' ')
$psi.Environment['PATH'] = $childPath
$psi.Environment['QT_PLUGIN_PATH'] = $qtPluginPath
$psi.Environment['QT_QPA_PLATFORM'] = 'offscreen'
$psi.Environment['QTSCRIPT_TEST_DLL'] = $quickJsScript
$evaluationTimeoutMilliseconds = 0
if ($Suite -eq 'qscriptjstestsuite') {
    $evaluationTimeoutMilliseconds = [Math]::Max(1000, (($TimeoutSeconds - 5) * 1000))
    $psi.Environment['QTSCRIPT_EVAL_TIMEOUT_MS'] = [string]$evaluationTimeoutMilliseconds
}

if ($IsolateFunctions) {
    # QtTest data-driven functions can cascade after one crashed or hung data
    # row.  Enumerate and execute each top-level function in a fresh process so
    # every function gets a bounded result and the parent can account for all
    # of them.  The child runner still records the actual DLL and command line.
    $savedPath = $env:PATH
    $savedPluginPath = $env:QT_PLUGIN_PATH
    $savedPlatform = $env:QT_QPA_PLATFORM
    try {
        $env:PATH = $childPath
        $env:QT_PLUGIN_PATH = $qtPluginPath
        $env:QT_QPA_PLATFORM = 'offscreen'
        $functionNames = @(& $testExecutable '-functions' 2>$null |
            ForEach-Object { ($_ -replace '\(\)$', '').Trim() } |
            Where-Object { $_ })
    } finally {
        $env:PATH = $savedPath
        if ($null -eq $savedPluginPath) { Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue }
        else { $env:QT_PLUGIN_PATH = $savedPluginPath }
        if ($null -eq $savedPlatform) { Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue }
        else { $env:QT_QPA_PLATFORM = $savedPlatform }
    }
    if ($functionNames.Count -eq 0) {
        throw "The test executable did not report any functions: $testExecutable"
    }

    $summaryPath = Join-Path $OutputDirectory "$name-isolated.tsv"
    $summary = [System.Collections.Generic.List[string]]::new()
    $summary.Add("function`tresult`texit_code`tpassed`tfailed`tskipped`tblacklisted")
    $failedFunctions = [System.Collections.Generic.List[string]]::new()
    foreach ($functionName in $functionNames) {
        $safeName = $functionName -replace '[^A-Za-z0-9_.-]', '_'
        $functionDirectory = Join-Path $OutputDirectory $safeName
        New-Item -ItemType Directory -Path $functionDirectory -Force | Out-Null
        $childLog = Join-Path $functionDirectory 'runner.log.txt'
        & $PSCommandPath `
            -BuildDir $BuildDir `
            -QtBin $QtBin `
            -Configuration $Configuration `
            -Suite $Suite `
            -Function $functionName `
            -OutputDirectory $functionDirectory `
            -TimeoutSeconds $TimeoutSeconds `
            -AllowTestFailures *> $childLog

        $meta = @(Get-ChildItem -LiteralPath $functionDirectory -Filter '*.meta.txt' |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1)
        $result = 'runner-error'
        $exitCode = 'unknown'
        $stdoutFile = $null
        if ($meta.Count -eq 1) {
            $metaValues = @{}
            foreach ($line in Get-Content -LiteralPath $meta[0].FullName) {
                if ($line -match '^([^=]+)=(.*)$') { $metaValues[$Matches[1]] = $Matches[2] }
            }
            if ($metaValues.ContainsKey('result')) { $result = $metaValues['result'] }
            $exitCode = if ($metaValues.ContainsKey('exit_code')) { $metaValues['exit_code'] } else { 'unknown' }
            $stdoutFile = @(Get-ChildItem -LiteralPath $functionDirectory -Filter '*.stdout.txt' |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1)
        }
        $passed = 0; $failed = 0; $skipped = 0; $blacklisted = 0
        if ($stdoutFile -and (Test-Path -LiteralPath $stdoutFile[0].FullName)) {
            $totals = Get-Content -LiteralPath $stdoutFile[0].FullName |
                Where-Object { $_ -match '^Totals:' } | Select-Object -Last 1
            if ($totals -match '^Totals:\s+(\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted') {
                $passed = $Matches[1]; $failed = $Matches[2]
                $skipped = $Matches[3]; $blacklisted = $Matches[4]
                if ([int]$failed -gt 0) { $result = 'assertions' }
                elseif ($result -eq 'runner-error') { $result = 'pass' }
            }
        }
        if ($result -ne 'pass') {
            $failedFunctions.Add($functionName)
        }
        $summary.Add("$functionName`t$result`t$exitCode`t$passed`t$failed`t$skipped`t$blacklisted")
        Write-Output ("{0}: {1} (exit {2}, {3} passed / {4} failed / {5} skipped)" -f
                      $functionName, $result, $exitCode, $passed, $failed, $skipped)
    }
    $summary | Set-Content -LiteralPath $summaryPath
    Write-Output "isolated_summary=$summaryPath"
    Write-Output "isolated_functions=$($functionNames.Count)"
    Write-Output "isolated_failures=$($failedFunctions.Count)"
    if ($failedFunctions.Count -and -not $AllowTestFailures) {
        throw "Isolated inherited test functions failed: $($failedFunctions -join ', ')"
    }
    return
}

@(
    "suite=$Suite",
    "configuration=$Configuration",
    "executable=$testExecutable",
    "quickjs_dll=$quickJsScript",
    "quickjs_dll_sha256=$((Get-FileHash -Algorithm SHA256 -LiteralPath $quickJsScript).Hash)",
    "qt_bin=$QtBin",
    "function=$Function",
    "arguments=$($psi.Arguments)",
    "timeout_seconds=$TimeoutSeconds",
    "evaluation_timeout_ms=$evaluationTimeoutMilliseconds",
    "path_first=$($pathParts[0])",
    "started_utc=$([DateTime]::UtcNow.ToString('o'))"
) | Set-Content -LiteralPath $metadataPath

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $psi
if (-not $process.Start()) {
    throw "Unable to start inherited test: $testExecutable"
}
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$completed = $process.WaitForExit($TimeoutSeconds * 1000)
if (-not $completed) {
    try {
        if (-not $process.HasExited) {
            $process.Kill($true)
            [void]$process.WaitForExit(5000)
        }
    } catch { }
    if (-not $process.HasExited) {
        $taskkill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
        & $taskkill /PID $process.Id /T /F *> $null
        [void]$process.WaitForExit(5000)
    }
    $stdoutTask.GetAwaiter().GetResult() | Set-Content -LiteralPath $consolePath
    $stderrTask.GetAwaiter().GetResult() | Set-Content -LiteralPath $stderrPath
    if (-not (Test-Path -LiteralPath $stdoutPath)) {
        Copy-Item -LiteralPath $consolePath -Destination $stdoutPath
    }
    Add-Content -LiteralPath $metadataPath "result=timeout"
    Add-Content -LiteralPath $metadataPath 'exit_code=timeout'
    Add-Content -LiteralPath $metadataPath "finished_utc=$([DateTime]::UtcNow.ToString('o'))"
    Get-Content -LiteralPath $stdoutPath
    if (Test-Path -LiteralPath $stderrPath) {
        Get-Content -LiteralPath $stderrPath | ForEach-Object { Write-Host "[stderr] $_" }
    }
    if (-not $AllowTestFailures) {
        throw "Inherited test timed out after $TimeoutSeconds seconds: $Suite $Function"
    }
    return
}
$stdoutTask.GetAwaiter().GetResult() | Set-Content -LiteralPath $consolePath
$stderrTask.GetAwaiter().GetResult() | Set-Content -LiteralPath $stderrPath
if (-not (Test-Path -LiteralPath $stdoutPath)) {
    Copy-Item -LiteralPath $consolePath -Destination $stdoutPath
}
$exitCode = $process.ExitCode
Add-Content -LiteralPath $metadataPath "exit_code=$exitCode"
Add-Content -LiteralPath $metadataPath "finished_utc=$([DateTime]::UtcNow.ToString('o'))"

Get-Content -LiteralPath $stdoutPath
if (Test-Path -LiteralPath $stderrPath) {
    Get-Content -LiteralPath $stderrPath | ForEach-Object { Write-Host "[stderr] $_" }
}
if ($exitCode -ne 0 -and -not $AllowTestFailures) {
    throw "Inherited test failed with exit code $exitCode. Output: $stdoutPath"
}
