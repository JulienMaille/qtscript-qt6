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
    [ValidateSet('qscriptcontext', 'qscriptengine', 'qscriptvalue',
                 'qscriptextqobject', 'qscriptjstestsuite', 'qscriptv8testsuite')]
    [string] $Suite,

    [string] $OutputDirectory = '',

    [int] $TimeoutSeconds = 120,

    [switch] $AllowTestFailures
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$QtBin = [System.IO.Path]::GetFullPath($QtBin)
$scriptDirectory = Split-Path -Parent $PSCommandPath
$repositoryRoot = Split-Path -Parent $scriptDirectory
$runner = Join-Path $scriptDirectory 'run-inherited-tests.ps1'
if (-not (Test-Path -LiteralPath $runner)) {
    throw "The isolated runner was not found: $runner"
}

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot '.work\inherited-suite-runs'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDirectory = Join-Path $OutputDirectory "$Suite-$Configuration-$stamp"
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null

function Get-MetadataValue {
    param(
        [Parameter(Mandatory)][string[]] $Lines,
        [Parameter(Mandatory)][string] $Name
    )
    $line = $Lines | Where-Object { $_ -like "$Name=*" } | Select-Object -First 1
    if (-not $line) {
        return ''
    }
    return $line.Substring($Name.Length + 1)
}

function Find-LatestMetadata {
    param([Parameter(Mandatory)][string] $FunctionName)
    $safeName = $FunctionName -replace '[^A-Za-z0-9_.-]', '_'
    $metadata = Get-ChildItem -LiteralPath $runDirectory -Filter "*-$safeName.meta.txt" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $metadata) {
        throw "No metadata was produced for function: $FunctionName"
    }
    return $metadata
}

function Invoke-IsolatedFunction {
    param([Parameter(Mandatory)][string] $FunctionName)
    $consolePath = Join-Path $runDirectory ("invoke-" +
        ($FunctionName -replace '[^A-Za-z0-9_.-]', '_') + '.console.txt')
    & $runner `
        -BuildDir $BuildDir `
        -QtBin $QtBin `
        -Configuration $Configuration `
        -Suite $Suite `
        -Function $FunctionName `
        -OutputDirectory $runDirectory `
        -TimeoutSeconds $TimeoutSeconds `
        -AllowTestFailures *> $consolePath
    return Find-LatestMetadata $FunctionName
}

$discoveryConsole = Join-Path $runDirectory 'discovery.console.txt'
& $runner `
    -BuildDir $BuildDir `
    -QtBin $QtBin `
    -Configuration $Configuration `
    -Suite $Suite `
    -Function '-functions' `
    -OutputDirectory $runDirectory `
    -TimeoutSeconds $TimeoutSeconds `
    -AllowTestFailures *> $discoveryConsole
$discoveryMetadata = Find-LatestMetadata '-functions'
$discoveryStdout = $discoveryMetadata.FullName -replace '\.meta\.txt$', '.stdout.txt'
$functions = @(Get-Content -LiteralPath $discoveryStdout |
    Where-Object { $_ -match '^[A-Za-z_][A-Za-z0-9_./-]*\(\)$' } |
    ForEach-Object { $_.TrimEnd('(', ')') })
if ($functions.Count -eq 0) {
    throw "No QtTest functions were discovered for $Suite. See $discoveryStdout"
}

$records = [System.Collections.Generic.List[object]]::new()
foreach ($functionName in $functions) {
    $metadata = Invoke-IsolatedFunction $functionName
    $lines = Get-Content -LiteralPath $metadata.FullName
    $stdoutPath = $metadata.FullName -replace '\.meta\.txt$', '.stdout.txt'
    $stdout = Get-Content -LiteralPath $stdoutPath
    $exitCodeText = Get-MetadataValue $lines 'exit_code'
    $timedOut = $exitCodeText -eq 'timeout'
    $exitCode = 0
    [void][int]::TryParse($exitCodeText, [ref]$exitCode)
    $passCount = @($stdout | Where-Object { $_ -match '^PASS\s+' }).Count
    $failCount = @($stdout | Where-Object { $_ -match '^FAIL!\s+' }).Count
    $xpassCount = @($stdout | Where-Object { $_ -match '^XPASS\s+' }).Count
    $xfailCount = @($stdout | Where-Object { $_ -match '^XFAIL\s+' }).Count
    $skipCount = @($stdout | Where-Object { $_ -match '^SKIP\s+' }).Count
    $totals = @($stdout | Where-Object { $_ -match '^Totals:' })
    $failedTests = 0
    $passedTests = 0
    if ($totals.Count -ne 0) {
        $totalText = $totals -join ' '
        $failedMatch = [regex]::Match($totalText, '(\d+) failed')
        $passedMatch = [regex]::Match($totalText, '(\d+) passed')
        if ($failedMatch.Success) { $failedTests = [int]$failedMatch.Groups[1].Value }
        if ($passedMatch.Success) { $passedTests = [int]$passedMatch.Groups[1].Value }
    }
    $status = if ($failCount -ne 0 -or $failedTests -ne 0) {
        'Fail'
    } elseif ($timedOut -or $exitCode -ne 0) {
        'CrashOrTimeout'
    } elseif ($xpassCount -ne 0) {
        'PassWithXPass'
    } elseif ($skipCount -ne 0 -and $passCount -eq 0) {
        'Skip'
    } else {
        'Pass'
    }
    $records.Add([pscustomobject]@{
        Suite = $Suite
        Configuration = $Configuration
        Function = $functionName
        Status = $status
        ExitCode = $exitCodeText
        PassLines = $passCount
        FailLines = $failCount
        XPassLines = $xpassCount
        XFailLines = $xfailCount
        SkipLines = $skipCount
        PassedTests = $passedTests
        FailedTests = $failedTests
        Totals = ($totals -join ' ')
        Stdout = $stdoutPath
        Metadata = $metadata.FullName
    })
}

$summaryPath = Join-Path $runDirectory 'summary.csv'
$records | Export-Csv -LiteralPath $summaryPath -NoTypeInformation -Encoding UTF8
$failureReportPath = Join-Path $runDirectory 'failures.txt'
$records | Where-Object { $_.Status -ne 'Pass' } |
    ForEach-Object { "$($_.Function): $($_.Status) exit=$($_.ExitCode) log=$($_.Stdout)" } |
    Set-Content -LiteralPath $failureReportPath

$statusCounts = $records | Group-Object Status | Sort-Object Name |
    ForEach-Object { "$($_.Name)=$($_.Count)" }
Write-Output "suite=$Suite functions=$($records.Count) $($statusCounts -join ', ')"
Write-Output "summary=$summaryPath"
Write-Output "failures=$failureReportPath"

$badRecords = @($records | Where-Object { $_.Status -eq 'Fail' -or $_.Status -eq 'CrashOrTimeout' })
if ($badRecords.Count -ne 0 -and -not $AllowTestFailures) {
    throw "$($badRecords.Count) isolated functions failed. See $failureReportPath"
}
