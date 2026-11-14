param(
    [string]$UE5Root = "C:\Program Files\Epic Games\UE_5.8",
    [string]$Project = "",
    [int]$Seconds = 20,
    [int]$Warmup = 5,
    [int]$Runs = 3,
    [ValidateSet("all", "dynamic", "ndgi", "decode", "bc7", "pool")]
    [string]$Mode = "all",
    [string]$NDGIMap = "/Game/NDGI/Bench/Lvl_Bench_NDGI",
    [string]$DynamicMap = "/Game/NDGI/Bench/Lvl_Bench_Dynamic",
    [string]$StreamingMap = "/Game/NDGI/NDGITestBakeMap_WP",
    [int[]]$ResidentPools = @(0, 8, 16, 32),
    [int]$DecodeIterations = 200,
    [int]$ResX = 1920,
    [int]$ResY = 1080
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "NDGITools.ps1")

$Project = Resolve-NDGIProject -Project $Project
$ProjectRoot = Split-Path -Parent $Project
$EditorCmd = Join-Path $UE5Root "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path -LiteralPath $EditorCmd)) {
    throw "UnrealEditor-Cmd.exe not found: $EditorCmd"
}

$LogDir = Join-Path $ProjectRoot "Saved\Logs"
$CsvDir = Join-Path $ProjectRoot "Saved\NDGI\Benchmarks"
$SummaryPath = Join-Path $CsvDir "summary.json"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $CsvDir | Out-Null

$Rows = New-Object System.Collections.Generic.List[object]
$Failures = New-Object System.Collections.Generic.List[string]

function Add-NDGIBenchRow {
    param(
        [string]$Suite,
        [string]$Label,
        [double]$P50Ms = [double]::NaN,
        [double]$P95Ms = [double]::NaN,
        [double]$CpuMB = [double]::NaN,
        [double]$GpuMB = [double]::NaN,
        [int]$Models = -1,
        [int]$Skipped = -1,
        [double]$Extra = [double]::NaN,
        [string]$LogPath = ""
    )

    $Rows.Add([pscustomobject]@{
        Suite = $Suite
        Label = $Label
        P50Ms = $P50Ms
        P95Ms = $P95Ms
        CpuMB = $CpuMB
        GpuMB = $GpuMB
        Models = $Models
        Skipped = $Skipped
        Extra = $Extra
        Log = $LogPath
    })
}

function Invoke-NDGIBenchRun {
    param(
        [string]$Map,
        [string]$ExecCmds,
        [string]$Tag,
        [switch]$Game,
        [int]$TimeoutMinutes = 30
    )

    $LogPath = Join-Path $LogDir "NDGI_$Tag.log"
    Write-Host ""
    Write-Host "=== $Tag ==="
    Write-Host "Map: $Map"
    Write-Host "ExecCmds: $ExecCmds"

    $Result = Invoke-NDGIUnreal -EditorCmd $EditorCmd -Project $Project -Map $Map `
        -ExecCmds $ExecCmds -LogPath $LogPath -Game:$Game -ResX $ResX -ResY $ResY `
        -TimeoutMinutes $TimeoutMinutes
    if ($Result.TimedOut) {
        throw "Benchmark run '$Tag' timed out after $TimeoutMinutes minutes (see $LogPath)."
    }

    Get-Content -LiteralPath $LogPath |
        Select-String -Pattern "LogNDGIRuntime|LogNDGI|BenchmarkDecode|TestDecode|TestAsync|TestVTStreaming|TestDiskStreaming|stale-asset" |
        ForEach-Object { Write-Host $_.Line.Trim() }

    $Failed = Get-NDGILogMatch -Path $LogPath -Pattern "\bFAILED\b" -Tail 4
    if ($Failed.Count -gt 0) {
        $Failures.Add("$Tag`: $($Failed[-1].Line.Trim())")
    }
    return $LogPath
}

function Get-NDGIBenchNumber {
    param([string]$LogPath, [string]$Pattern, [double]$Default = [double]::NaN)
    return Get-NDGILogNumber -Path $LogPath -Pattern $Pattern -Default $Default
}

if ($Mode -in @("all", "dynamic")) {
    for ($Index = 1; $Index -le $Runs; $Index++) {
        $Tag = "dynamic_run$Index"
        $Exec = "r.VSync 0,t.MaxFPS 0,r.DynamicGlobalIlluminationMethod 1,r.ReflectionMethod 1,ndgi.Benchmark $Seconds $Warmup $Tag quit"
        $Log = Invoke-NDGIBenchRun -Map $DynamicMap -ExecCmds $Exec -Tag $Tag -Game
        Add-NDGIBenchRow -Suite "dynamic" -Label $Tag -LogPath $Log
    }
}

if ($Mode -in @("all", "ndgi")) {
    for ($Index = 1; $Index -le $Runs; $Index++) {
        $Tag = "ndgi_run$Index"
        $Exec = "r.VSync 0,t.MaxFPS 0,r.DynamicGlobalIlluminationMethod 1,r.ReflectionMethod 1,ndgi.BenchmarkNDGI 1.0 $Seconds $Warmup $Tag quit"
        $Log = Invoke-NDGIBenchRun -Map $NDGIMap -ExecCmds $Exec -Tag $Tag -Game
        Add-NDGIBenchRow -Suite "ndgi" -Label $Tag -LogPath $Log
    }
}

if ($Mode -in @("all", "decode")) {
    $Tag = "decode"
    $Exec = "r.VSync 0,t.MaxFPS 0,ndgi.BenchmarkDecode $DecodeIterations quit"
    $Log = Invoke-NDGIBenchRun -Map $NDGIMap -ExecCmds $Exec -Tag $Tag -Game
    Add-NDGIBenchRow -Suite "decode" -Label $Tag -LogPath $Log
}

if ($Mode -in @("all", "bc7")) {
    $Tag = "bc7_warm"
    $Exec = "r.VSync 0,t.MaxFPS 0,ndgi.BenchmarkDecodeWarm $DecodeIterations quit"
    $Log = Invoke-NDGIBenchRun -Map $NDGIMap -ExecCmds $Exec -Tag $Tag -Game
    $P50 = Get-NDGIBenchNumber -LogPath $Log -Pattern "Engine lightmaps total .* p50 ([\d.]+) ms"
    $P95 = Get-NDGIBenchNumber -LogPath $Log -Pattern "live engine-lightmap p95 ([\d.]+) ms"
    $Models = Get-NDGIBenchNumber -LogPath $Log -Pattern "BenchmarkDecodeWarm: map .* ([\d]+) model\(s\)"
    $Skipped = Get-NDGIBenchNumber -LogPath $Log -Pattern "([\d]+) model\(s\) skipped" -Default 0
    $Log2 = Invoke-NDGIBenchRun -Map $NDGIMap -ExecCmds "r.VSync 0,t.MaxFPS 0,ndgi.TestAsyncCache quit" -Tag "bc7_async_cache" -Game
    $CpuMB = Get-NDGIBenchNumber -LogPath $Log2 -Pattern "TestAsyncCache: .* CPU ([\d.]+) MB"
    $GpuMB = Get-NDGIBenchNumber -LogPath $Log2 -Pattern "TestAsyncCache: .* GPU ([\d.]+) MB"
    $AsyncTick = Get-NDGIBenchNumber -LogPath $Log2 -Pattern "max tick ([\d.]+) ms"
    Add-NDGIBenchRow -Suite "bc7" -Label "warm decode" -P50Ms $P50 -P95Ms $P95 `
        -CpuMB $CpuMB -GpuMB $GpuMB -Models $Models -Skipped $Skipped -Extra $AsyncTick -LogPath $Log

    $Log = Invoke-NDGIBenchRun -Map $NDGIMap -ExecCmds "r.VSync 0,t.MaxFPS 0,ndgi.TestDecodeCadence 5 quit" -Tag "bc7_cadence" -Game
    $Cadence = Get-NDGIBenchNumber -LogPath $Log -Pattern "\(([\d.]+) Hz, target 10-20 Hz\)"
    Add-NDGIBenchRow -Suite "bc7" -Label "cadence (Hz)" -Extra $Cadence -LogPath $Log

    $Log = Invoke-NDGIBenchRun -Map $NDGIMap -ExecCmds "r.VSync 0,t.MaxFPS 0,ndgi.ReportStorage quit" -Tag "bc7_storage" -Game
    $Bpp = Get-NDGIBenchNumber -LogPath $Log -Pattern "BC7 BPP ([\d.]+) bits"
    $RawRatio = Get-NDGIBenchNumber -LogPath $Log -Pattern "BC7/raw-TOD-set ratio ([\d.]+)x"
    Add-NDGIBenchRow -Suite "bc7" -Label "storage" -Extra $Bpp -LogPath $Log

    # Streaming tests use a map with BC7-trained cell-owned models.
    $StreamingCommands = @(
        @{ Tag = "bc7_vt_streaming"; Exec = "ndgi.TestVTStreaming 2 quit" },
        @{ Tag = "bc7_disk_streaming"; Exec = "ndgi.TestDiskStreaming 2 quit" },
        @{ Tag = "bc7_async_pages"; Exec = "ndgi.TestAsyncPages 2 quit" }
    )
    foreach ($StreamingCommand in $StreamingCommands) {
        $Log = Invoke-NDGIBenchRun -Map $StreamingMap -ExecCmds $StreamingCommand.Exec -Tag $StreamingCommand.Tag -Game
        Add-NDGIBenchRow -Suite "streaming" -Label $StreamingCommand.Tag -LogPath $Log
    }
}

if (($Mode -eq "all" -or $Mode -eq "bc7" -or $Mode -eq "pool") -and $ResidentPools.Count -gt 0) {
    foreach ($PoolSize in $ResidentPools) {
        $Tag = "pool_$PoolSize"
        $Exec = "r.VSync 0,t.MaxFPS 0,ndgi.ResidentModelsPerAtlas $PoolSize,ndgi.BenchmarkDecodeWarm $DecodeIterations quit"
        $Log = Invoke-NDGIBenchRun -Map $NDGIMap -ExecCmds $Exec -Tag $Tag -Game
        $P50 = Get-NDGIBenchNumber -LogPath $Log -Pattern "Engine lightmaps total .* p50 ([\d.]+) ms"
        $P95 = Get-NDGIBenchNumber -LogPath $Log -Pattern "live engine-lightmap p95 ([\d.]+) ms"
        $Models = Get-NDGIBenchNumber -LogPath $Log -Pattern "BenchmarkDecodeWarm: map .* ([\d]+) model\(s\)"
        $Skipped = Get-NDGIBenchNumber -LogPath $Log -Pattern "([\d]+) model\(s\) skipped" -Default 0
        $GpuMB = Get-NDGIBenchNumber -LogPath $Log -Pattern "BenchmarkDecodeWarm: map .* GPU ([\d.]+) MB"
        Add-NDGIBenchRow -Suite "pool" -Label "resident $PoolSize" -P50Ms $P50 -P95Ms $P95 `
            -GpuMB $GpuMB -Models $Models -Skipped $Skipped -Extra $PoolSize -LogPath $Log
    }
}

$Rows | Format-Table -AutoSize | Out-String | Write-Host
$Rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $SummaryPath
Write-Host ""
Write-Host "Summary: $SummaryPath"
Write-Host "Logs: $LogDir"

if ($Failures.Count -gt 0) {
    Write-Host ""
    Write-Host "NDGI benchmark FAILED ($($Failures.Count) run(s)):"
    $Failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
