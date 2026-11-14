param(
    [string]$UE5Root = "C:\Program Files\Epic Games\UE_5.8",
    [string]$Project = "",
    [string]$QualityMap = "/Game/NDGI/NDGITestBakeMap",
    [string]$BenchmarkMap = "/Game/NDGI/Bench/Lvl_Bench_NDGI",
    [ValidateSet("all", "quality", "perf", "memory", "holdout", "packaged")]
    [string]$Mode = "all",
    [string]$HoldoutMap = "",
    [int]$HoldoutTiles = 3,
    [int]$HoldoutSteps = 3000,
    [double]$MinHoldoutPsnr = 10.0,
    [switch]$SkipBuild,
    [switch]$SkipPackaged,
    [switch]$SkipPackageBuild,
    [string]$PackageArchiveDirectory = "",
    [string]$PackageStagingDirectory = "",
    [ValidateSet("Development", "Shipping")]
    [string]$ClientConfig = "Development",
    [double]$MinSHPsnr = 20.0,
    [double]$MaxDirectionalityMean = 0.02,
    [double]$MaxDirectionalityP99 = 0.10,
    [double]$MaxDirectionalityP999 = 0.20,
    [double]$MaxDirectionalityMax = 0.60,
    [double]$MaxSeamStep = 0.0625,
    [double]$MaxContinuousSH = 0.02,
    [double]$MaxP95Ms = 50.0,
    [double]$MaxCacheTickMs = 100.0,
    [int]$MemoryBudgetMB = 2048
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "NDGITools.ps1")

$Project = Resolve-NDGIProject -Project $Project
$ProjectRoot = Split-Path -Parent $Project
if (-not $HoldoutMap) {
    $HoldoutMap = $QualityMap
}
$EditorCmd = Join-Path $UE5Root "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$BuildBat = Join-Path $UE5Root "Engine\Build\BatchFiles\Build.bat"
if (-not (Test-Path -LiteralPath $EditorCmd)) {
    throw "UnrealEditor-Cmd.exe not found under UE5Root: $EditorCmd"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$RunDirectory = Join-Path $ProjectRoot "Saved\NDGI\CI\$Stamp"
$LogDirectory = Join-Path $RunDirectory "Logs"
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null

$Results = New-Object System.Collections.Generic.List[object]
$Failures = New-Object System.Collections.Generic.List[string]
$SummaryFile = Join-Path $RunDirectory "summary.txt"
$SummaryJson = Join-Path $RunDirectory "summary.json"

function Add-NDGIResult {
    param(
        [string]$Stage,
        [string]$Status,
        [string]$Detail,
        [string]$LogPath
    )

    $Results.Add([pscustomobject]@{
        Stage = $Stage
        Status = $Status
        Detail = $Detail
        Log = $LogPath
    })
    if ($Status -ne "PASS") {
        $Failures.Add("$Stage`: $Detail")
    }
}

function Show-NDGILogTail {
    param([string]$Path, [int]$Count = 30)

    if (Test-Path -LiteralPath $Path) {
        Get-Content -LiteralPath $Path -Tail $Count |
            Where-Object { $_ -match "NDGI|Error|Warning|FAILED|PASSED|Fatal|Assertion" } |
            ForEach-Object { Write-Host "    $_" }
    }
}

function Invoke-NDGIGate {
    param(
        [string]$Name,
        [string]$Map,
        [string]$ExecCmds,
        [switch]$Game,
        [int]$TimeoutMinutes = 30
    )

    $LogPath = Join-Path $LogDirectory "$Name.log"
    Write-Host ""
    Write-Host "=== $Name ==="
    Write-Host "Map: $Map"
    Write-Host "ExecCmds: $ExecCmds"

    $Result = Invoke-NDGIUnreal -EditorCmd $EditorCmd -Project $Project -Map $Map `
        -ExecCmds $ExecCmds -LogPath $LogPath -Game:$Game -TimeoutMinutes $TimeoutMinutes

    if ($Result.TimedOut) {
        Add-NDGIResult -Stage $Name -Status "FAIL" -Detail "timed out after $TimeoutMinutes minutes" -LogPath $LogPath
        Show-NDGILogTail -Path $LogPath
        return $null
    }

    $Passed = Get-NDGILogMatch -Path $LogPath -Pattern "\bPASSED\b" -Tail 4
    $Failed = Get-NDGILogMatch -Path $LogPath -Pattern "\bFAILED\b|Fatal error|Assertion failed|LowLevelFatalError" -Tail 8 -CaseSensitive
    if ($Passed.Count -eq 0 -or $Failed.Count -gt 0) {
        $Detail = if ($Failed.Count -gt 0) { $Failed[-1].Line.Trim() } else { "no PASSED line in the log" }
        Add-NDGIResult -Stage $Name -Status "FAIL" -Detail $Detail -LogPath $LogPath
        Show-NDGILogTail -Path $LogPath
        return $null
    }

    Add-NDGIResult -Stage $Name -Status "PASS" -Detail "PASSED" -LogPath $LogPath
    return [pscustomobject]@{ LogPath = $LogPath; Result = $Result }
}

function Invoke-NDGIExpectedFailure {
    param(
        [string]$Name,
        [string]$Map,
        [string]$ExecCmds,
        [string]$ExpectedPattern,
        [switch]$Game,
        [int]$TimeoutMinutes = 30
    )

    $LogPath = Join-Path $LogDirectory "$Name.log"
    Write-Host ""
    Write-Host "=== $Name (expected failure) ==="
    Write-Host "Map: $Map"
    Write-Host "ExecCmds: $ExecCmds"

    $Result = Invoke-NDGIUnreal -EditorCmd $EditorCmd -Project $Project -Map $Map `
        -ExecCmds $ExecCmds -LogPath $LogPath -Game:$Game -TimeoutMinutes $TimeoutMinutes
    $Expected = Get-NDGILogMatch -Path $LogPath -Pattern $ExpectedPattern -Tail 2
    if ($Result.TimedOut -or $Expected.Count -eq 0) {
        Add-NDGIResult -Stage $Name -Status "FAIL" -Detail "expected rejection was not observed" -LogPath $LogPath
        Show-NDGILogTail -Path $LogPath
        return
    }
    Add-NDGIResult -Stage $Name -Status "PASS" -Detail "guard rejected the oversized cache" -LogPath $LogPath
}

if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== Build editor target ==="
    $BuildLog = Join-Path $LogDirectory "BuildEditor.log"
    $BuildResult = Invoke-NDGIBatch -Executable $BuildBat `
        -Arguments @("RPG_PrototypeEditor", "Win64", "Development", "-Project=$Project", "-WaitMutex", "-NoHotReload") `
        -LogPath $BuildLog -WorkingDirectory $ProjectRoot -TimeoutMinutes 45
    if ($BuildResult.TimedOut -or $BuildResult.ExitCode -ne 0) {
        Add-NDGIResult -Stage "BuildEditor" -Status "FAIL" -Detail "exit code $($BuildResult.ExitCode)" -LogPath $BuildLog
        Show-NDGILogTail -Path $BuildLog
    } else {
        Add-NDGIResult -Stage "BuildEditor" -Status "PASS" -Detail "editor target built" -LogPath $BuildLog
    }
}

if (($Mode -eq "all" -or $Mode -eq "quality") -and $Failures.Count -eq 0) {
    Invoke-NDGIGate -Name "TestGPU" -Map $QualityMap -ExecCmds "ndgi.TestGPU 1337 512 quit" | Out-Null
    Invoke-NDGIGate -Name "TestTrainGPU" -Map $QualityMap -ExecCmds "ndgi.TestTrainGPU 1337 200 2 quit" | Out-Null
    Invoke-NDGIGate -Name "TestLightmapSet" -Map $QualityMap -ExecCmds "ndgi.TestLightmapSet 1500 quit" | Out-Null
    Invoke-NDGIGate -Name "TestRuntimeDecode" -Map $QualityMap -ExecCmds "ndgi.TestRuntimeDecode 512 quit" | Out-Null
    Invoke-NDGIGate -Name "TestCachedDecodeMatch" -Map $QualityMap -ExecCmds "ndgi.TestCachedDecodeMatch 1.0 quit" | Out-Null
    Invoke-NDGIGate -Name "TestBC7" -Map $QualityMap -ExecCmds "ndgi.TestBC7 quit" | Out-Null
    Invoke-NDGIGate -Name "ReportStorage" -Map $QualityMap -ExecCmds "ndgi.ReportStorage quit" | Out-Null
    # Streaming gates require BC7-trained level assets. Run them manually after
    # retraining the benchmark map (ndgi.TestVTStreaming / TestDiskStreaming /
    # TestAsyncPages / VerifyStreamingDeps).

    $Sh = Invoke-NDGIGate -Name "ValidateSH" -Map $QualityMap -ExecCmds "ndgi.ValidateSH 1.0 quit"
    if ($Sh) {
        $Psnr = Get-NDGILogNumber -Path $Sh.LogPath -Pattern "ValidateSH: time .* overall PSNR ([\d.]+) dB"
        $OutOfRange = Get-NDGILogNumber -Path $Sh.LogPath -Pattern "ValidateSH: time .* out-of-range ([\d]+)" -Default 1
        $Detail = ("PSNR {0:N2} dB, out-of-range {1}" -f $Psnr, $OutOfRange)
        if ([double]::IsNaN($Psnr) -or $Psnr -lt $MinSHPsnr -or $OutOfRange -ne 0) {
            $Results[$Results.Count - 1].Status = "FAIL"
            $Results[$Results.Count - 1].Detail = $Detail
            $Failures.Add("ValidateSH: $Detail")
        } else {
            $Results[$Results.Count - 1].Detail = $Detail
        }
    }

    $Directionality = Invoke-NDGIGate -Name "ValidateDirectionality" -Map $QualityMap -ExecCmds "ndgi.ValidateDirectionality 1.0 quit"
    if ($Directionality) {
        $Mean = Get-NDGILogNumber -Path $Directionality.LogPath -Pattern "directionality mean \|err\| ([\d.]+)"
        $Max = Get-NDGILogNumber -Path $Directionality.LogPath -Pattern "directionality mean \|err\| [\d.]+, max \|err\| ([\d.]+)"
        $P99 = Get-NDGILogNumber -Path $Directionality.LogPath -Pattern "p99 ([\d.]+) p99\.9"
        $P999 = Get-NDGILogNumber -Path $Directionality.LogPath -Pattern "p99\.9 ([\d.]+)"
        $Detail = ("mean {0:N4}, p99 {1:N4}, p99.9 {2:N4}, max {3:N4}" -f $Mean, $P99, $P999, $Max)
        if ([double]::IsNaN($Mean) -or $Mean -ge $MaxDirectionalityMean -or
            $P99 -ge $MaxDirectionalityP99 -or $P999 -ge $MaxDirectionalityP999 -or
            $Max -ge $MaxDirectionalityMax) {
            $Results[$Results.Count - 1].Status = "FAIL"
            $Results[$Results.Count - 1].Detail = $Detail
            $Failures.Add("ValidateDirectionality: $Detail")
        } else {
            $Results[$Results.Count - 1].Detail = $Detail
        }
    }

    $Seams = Invoke-NDGIGate -Name "ValidateSeams" -Map $QualityMap -ExecCmds "ndgi.ValidateSeams 1.0 quit"
    if ($Seams) {
        $Lum = Get-NDGILogNumber -Path $Seams.LogPath -Pattern "ValidateSeams gate: luminance introduced ([\d]+)" -Default 1
        $ShIntroduced = Get-NDGILogNumber -Path $Seams.LogPath -Pattern "SH introduced ([\d]+)/" -Default 1
        $ShCount = Get-NDGILogNumber -Path $Seams.LogPath -Pattern "SH introduced [\d]+/([\d]+)" -Default 0
        $ShMax = Get-NDGILogNumber -Path $Seams.LogPath -Pattern "SH max introduced ([\d.]+)" -Default 1
        $ContinuousSh = Get-NDGILogNumber -Path $Seams.LogPath -Pattern "continuous SH mean ([\d.]+)" -Default 1
        $AllowedIntroduced = [Math]::Max(1, [Math]::Floor($ShCount * 0.005))
        $Detail = ("luminance introduced {0}, SH introduced {1}/{2}, SH max {3:N4}, continuous SH {4:N5}" -f
            $Lum, $ShIntroduced, $ShCount, $ShMax, $ContinuousSh)
        if ($Lum -ne 0 -or $ShIntroduced -gt $AllowedIntroduced -or
            $ShMax -gt $MaxSeamStep -or $ContinuousSh -gt $MaxContinuousSH) {
            $Results[$Results.Count - 1].Status = "FAIL"
            $Results[$Results.Count - 1].Detail = $Detail
            $Failures.Add("ValidateSeams: $Detail")
        } else {
            $Results[$Results.Count - 1].Detail = $Detail
        }
    }

    Invoke-NDGIGate -Name "ValidateLevelAsset" -Map $QualityMap -ExecCmds "ndgi.ValidateLevelAsset 1.0 quit" | Out-Null
}

if (($Mode -eq "all" -or $Mode -eq "perf") -and $Failures.Count -eq 0) {
    $Perf = Invoke-NDGIGate -Name "BenchmarkDecodeWarm" -Map $BenchmarkMap -ExecCmds "ndgi.BenchmarkDecodeWarm 200 quit" -Game -TimeoutMinutes 20
    if ($Perf) {
        $P95 = Get-NDGILogNumber -Path $Perf.LogPath -Pattern "live engine-lightmap p95 ([\d.]+) ms"
        $Skipped = Get-NDGILogNumber -Path $Perf.LogPath -Pattern "live engine-lightmap p95 [\d.]+ ms .* ([\d]+) model\(s\) skipped" -Default 0
        $Detail = ("engine-lightmap p95 {0:N2} ms (gate <= {1:N2} ms), skipped {2}" -f $P95, $MaxP95Ms, $Skipped)
        if ([double]::IsNaN($P95) -or $P95 -gt $MaxP95Ms -or $Skipped -ne 0) {
            $Results[$Results.Count - 1].Status = "FAIL"
            $Results[$Results.Count - 1].Detail = $Detail
            $Failures.Add("BenchmarkDecodeWarm: $Detail")
        } else {
            $Results[$Results.Count - 1].Detail = $Detail
        }
    }
    Invoke-NDGIGate -Name "TestDecodeCadence" -Map $BenchmarkMap -ExecCmds "ndgi.TestDecodeCadence 5 quit" -Game -TimeoutMinutes 20 | Out-Null
}

if (($Mode -eq "all" -or $Mode -eq "memory") -and $Failures.Count -eq 0) {
    $Memory = Invoke-NDGIGate -Name "TestAsyncCache" -Map $BenchmarkMap -ExecCmds "ndgi.TestAsyncCache quit" -Game -TimeoutMinutes 20
    if ($Memory) {
        $CpuMB = Get-NDGILogNumber -Path $Memory.LogPath -Pattern "CPU ([\d.]+) MB"
        $GpuMB = Get-NDGILogNumber -Path $Memory.LogPath -Pattern "GPU ([\d.]+) MB"
        $MaxTick = Get-NDGILogNumber -Path $Memory.LogPath -Pattern "max tick ([\d.]+) ms"
        $TotalMB = $CpuMB + $GpuMB
        $Detail = ("CPU {0:N1} MB + GPU {1:N1} MB = {2:N1} MB (budget {3} MB), max tick {4:N2} ms (gate <= {5:N2} ms)" -f
            $CpuMB, $GpuMB, $TotalMB, $MemoryBudgetMB, $MaxTick, $MaxCacheTickMs)
        if ([double]::IsNaN($TotalMB) -or $TotalMB -gt $MemoryBudgetMB -or
            [double]::IsNaN($MaxTick) -or $MaxTick -gt $MaxCacheTickMs) {
            $Results[$Results.Count - 1].Status = "FAIL"
            $Results[$Results.Count - 1].Detail = $Detail
            $Failures.Add("TestAsyncCache: $Detail")
        } else {
            $Results[$Results.Count - 1].Detail = $Detail
        }
    }
    Invoke-NDGIExpectedFailure -Name "TestCacheMemoryGuard" -Map $BenchmarkMap `
        -ExecCmds "ndgi.MaxCacheMemoryMB 1,ndgi.TestAsyncCache quit" `
        -ExpectedPattern "over the 1 MB limit" -Game -TimeoutMinutes 20
}

if ($Mode -eq "holdout" -and $Failures.Count -eq 0) {
    $Holdout = Invoke-NDGIGate -Name "ValidateTemporalHoldout" -Map $HoldoutMap `
        -ExecCmds "ndgi.ValidateTemporalHoldout 128 $HoldoutSteps 8 -1 $HoldoutTiles quit" -TimeoutMinutes 60
    if ($Holdout) {
        $Psnr = Get-NDGILogNumber -Path $Holdout.LogPath -Pattern "held-out slice PSNR ([\d.]+) dB"
        $Tiles = Get-NDGILogNumber -Path $Holdout.LogPath -Pattern "ValidateTemporalHoldout: .* ([\d]+) tile\(s\) across" -Default 0
        $Detail = ("held-out slice PSNR {0:N2} dB over {1} tile(s), {2} train steps (gate >= {3:N2} dB)" -f
            $Psnr, $Tiles, $HoldoutSteps, $MinHoldoutPsnr)
        if ([double]::IsNaN($Psnr) -or $Psnr -lt $MinHoldoutPsnr -or $Tiles -le 0) {
            $Results[$Results.Count - 1].Status = "FAIL"
            $Results[$Results.Count - 1].Detail = $Detail
            $Failures.Add("ValidateTemporalHoldout: $Detail")
        } else {
            $Results[$Results.Count - 1].Detail = $Detail
        }
    }
}

if (($Mode -eq "all" -or $Mode -eq "quality") -and $Failures.Count -eq 0) {
    $Smoke = Invoke-NDGIGate -Name "TestRuntimeSmokeEditorGame" -Map $QualityMap `
        -ExecCmds "ndgi.TestRuntimeSmoke 0.5 1.0 quit" -Game -TimeoutMinutes 20
    if ($Smoke) {
        $Results[$Results.Count - 1].Detail = "PIE/game-mode runtime decode smoke passed"
    }
}

if (($Mode -eq "all" -or $Mode -eq "packaged") -and -not $SkipPackaged -and $Failures.Count -eq 0) {
    Write-Host ""
    Write-Host "=== Packaged build and smoke ==="
    $PackageScript = Join-Path $PSScriptRoot "RunNDGIPackage.ps1"
    $PackageArchive = if ($PackageArchiveDirectory) { $PackageArchiveDirectory } else { Join-Path $RunDirectory "Package" }
    $PackageArgs = @{
        UE5Root = $UE5Root
        Project = $Project
        Map = $QualityMap
        ClientConfig = $ClientConfig
        ArchiveDirectory = $PackageArchive
        TimeA = 0.5
        TimeB = 1.0
    }
    if ($PackageStagingDirectory) {
        $PackageArgs.StagingDirectory = $PackageStagingDirectory
    }
    if ($SkipPackageBuild) {
        $PackageArgs.SkipBuild = $true
    }
    try {
        & $PackageScript @PackageArgs
        Add-NDGIResult -Stage "PackagedBuild" -Status "PASS" -Detail "cooked DX12 package + packaged smoke passed" -LogPath (Join-Path $PackageArchive "Logs\PackagedSmoke.log")
    } catch {
        Add-NDGIResult -Stage "PackagedBuild" -Status "FAIL" -Detail $_.Exception.Message -LogPath (Join-Path $PackageArchive "Logs\PackagedSmoke.log")
        Show-NDGILogTail -Path (Join-Path $PackageArchive "Logs\BuildCookRun.log") -Count 40
        Show-NDGILogTail -Path (Join-Path $PackageArchive "Logs\PackagedSmoke.log") -Count 40
    }
}

$Results | Format-Table -AutoSize | Out-String | Write-Host

$SummaryText = $Results | Format-Table -AutoSize | Out-String
$SummaryText | Set-Content -LiteralPath $SummaryFile
$Results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $SummaryJson

if ($Failures.Count -gt 0) {
    Write-Host ""
    Write-Host "NDGI CI FAILED ($($Failures.Count) gate(s)):"
    $Failures | ForEach-Object { Write-Host "  - $_" }
    Write-Host "Logs: $LogDirectory"
    exit 1
}

Write-Host ""
Write-Host "NDGI CI PASSED"
Write-Host "Logs: $LogDirectory"
exit 0
