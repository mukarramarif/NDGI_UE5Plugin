param(
    [string]$UE5Root = "C:\Program Files\Epic Games\UE_5.8",
    [string]$Project = "",
    [string]$Map = "/Game/NDGI/NDGITestBakeMap",
    [ValidateSet("Development", "Shipping")]
    [string]$ClientConfig = "Development",
    [string]$ArchiveDirectory = "",
    [string]$StagingDirectory = "",
    [float]$TimeA = 0.5,
    [float]$TimeB = 1.0,
    [switch]$SkipBuild,
    [switch]$SkipSmoke,
    [switch]$SkipQuarantine,
    [string[]]$QuarantineContent = @(
        "Content\SoulCity\Maps\LV_Soul_Slum.umap",
        "Content\SoulCity\Maps\SLV_Soul_Slum_Collision.umap"
    ),
    [int]$TimeoutMinutes = 90,
    [int]$SmokeTimeoutMinutes = 15
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "NDGITools.ps1")

$Project = Resolve-NDGIProject -Project $Project
$ProjectRoot = Split-Path -Parent $Project
$ProjectName = [System.IO.Path]::GetFileNameWithoutExtension($Project)
$RunUAT = Join-Path $UE5Root "Engine\Build\BatchFiles\RunUAT.bat"
if (-not (Test-Path -LiteralPath $RunUAT)) {
    throw "RunUAT.bat not found under UE5Root: $RunUAT"
}

if (-not $ArchiveDirectory) {
    $Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $ArchiveDirectory = Join-Path $ProjectRoot "Saved\NDGI\Packaged\${Stamp}_$ClientConfig"
}
$ArchiveDirectory = [System.IO.Path]::GetFullPath($ArchiveDirectory)
New-Item -ItemType Directory -Force -Path $ArchiveDirectory | Out-Null

$LogDirectory = Join-Path $ArchiveDirectory "Logs"
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
$CookLog = Join-Path $LogDirectory "BuildCookRun.log"
$SmokeLog = Join-Path $LogDirectory "PackagedSmoke.log"

if ($SkipBuild) {
    $GameExeName = if ($ClientConfig -eq "Shipping") {
        "$ProjectName-Win64-Shipping.exe"
    } else {
        "$ProjectName.exe"
    }
    $GameExe = Join-Path $ProjectRoot "Binaries\Win64\$GameExeName"
    if (-not (Test-Path -LiteralPath $GameExe)) {
        throw "-SkipBuild was requested, but the game executable is missing: $GameExe"
    }
}

$Drive = Get-PSDrive -Name ([System.IO.Path]::GetPathRoot($ArchiveDirectory).TrimEnd(":\"))
if ($Drive -and $Drive.Free -lt 8GB) {
    Write-Warning ("Only {0:N1} GB free on {1}: the cook can run out of disk." -f ($Drive.Free / 1GB), $Drive.Name)
}

Write-Host ""
Write-Host "=== NDGI packaged build ==="
Write-Host "Project:    $Project"
Write-Host "Map:        $Map"
Write-Host "Config:     $ClientConfig"
Write-Host "Archive:    $ArchiveDirectory"
Write-Host "Cook log:   $CookLog"

$UATArguments = @(
    "BuildCookRun",
    "-project=$Project",
    "-platform=Win64",
    "-clientconfig=$ClientConfig",
    "-map=$Map",
    "-cook",
    "-stage",
    "-pak",
    "-package",
    "-prereqs",
    "-archive",
    "-archivedirectory=$ArchiveDirectory",
    "-nodebuginfo",
    "-unattended",
    "-utf8output",
    "-nosound",
    "-NoP4"
)
if ($SkipBuild) {
    $UATArguments += "-skipbuild"
} else {
    $UATArguments += "-build"
}
if ($StagingDirectory) {
    $StagingDirectory = [System.IO.Path]::GetFullPath($StagingDirectory)
    New-Item -ItemType Directory -Force -Path $StagingDirectory | Out-Null
    $UATArguments += "-stagingdirectory=$StagingDirectory"
}


$QuarantineMoved = @()
if (-not $SkipQuarantine) {
    $QuarantineRoot = Join-Path $ProjectRoot ("Saved\NDGI\PackagingQuarantine\" + (Get-Date -Format "yyyyMMdd_HHmmss"))
    foreach ($RelativePath in $QuarantineContent) {
        $Source = Join-Path $ProjectRoot $RelativePath
        if (-not (Test-Path -LiteralPath $Source)) {
            continue
        }
        $Destination = Join-Path $QuarantineRoot $RelativePath
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Move-Item -LiteralPath $Source -Destination $Destination -Force
        $QuarantineMoved += [pscustomobject]@{ Source = $Source; Destination = $Destination }
        Write-Host "Quarantined known-unloadable content: $RelativePath"
    }
    if ($QuarantineMoved.Count -gt 0) {
        Write-Host "Quarantine folder: $QuarantineRoot (restored automatically when packaging exits)"
    }
}

try {
$CookResult = Invoke-NDGIBatch -Executable $RunUAT -Arguments $UATArguments -LogPath $CookLog `
    -WorkingDirectory $ProjectRoot -TimeoutMinutes $TimeoutMinutes
if ($CookResult.TimedOut) {
    throw "BuildCookRun timed out after $TimeoutMinutes minutes. See $CookLog"
}
if ($CookResult.ExitCode -ne 0) {
    $ErrorLines = Get-NDGILogMatch -Path $CookLog -Pattern "error:|Fatal error|BuildCookRun.*failed|AutomationTool.*failed" -Tail 12 -CaseSensitive
    foreach ($ErrorLine in $ErrorLines) {
        Write-Host $ErrorLine.Line
    }
    throw "BuildCookRun failed with exit code $($CookResult.ExitCode). See $CookLog"
}

$Executable = Get-ChildItem -LiteralPath $ArchiveDirectory -Recurse -File |
    Where-Object {
        $_.Name -like "$ProjectName*.exe" -and
        $_.FullName -match "\\Windows\\[^\\]+\\Binaries\\Win64\\"
    } |
    Sort-Object Length -Descending |
    Select-Object -First 1
if (-not $Executable) {
    # Fallback for unusual archive layouts; still avoid the tiny bootstrap exe.
    $Executable = Get-ChildItem -LiteralPath $ArchiveDirectory -Recurse -Filter "$ProjectName*.exe" -File |
        Where-Object { $_.FullName -notmatch "\\Engine\\" } |
        Sort-Object Length -Descending |
        Select-Object -First 1
}
if (-not $Executable) {
    throw "BuildCookRun succeeded, but no packaged executable was found under $ArchiveDirectory"
}

Write-Host "Executable: $($Executable.FullName)"

if ($SkipSmoke) {
    Write-Host "Packaged smoke test skipped (-SkipSmoke)."
    return
}

Write-Host ""
Write-Host "=== NDGI packaged smoke test ==="
Write-Host "Map:        $Map"
Write-Host "Trigger:    -ndgiSmoke -ndgiSmokeTimeA=$TimeA -ndgiSmokeTimeB=$TimeB"
Write-Host "Smoke log:  $SmokeLog"

# Shipping builds compile out UE_LOG output. The runtime writes this state file
# so the CI gate works in both Development and Shipping packages.
$LocalStatePath = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "$ProjectName\Saved\NDGI\TestRuntimeSmoke.txt"
Remove-Item -LiteralPath $LocalStatePath -Force -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $ArchiveDirectory -Recurse -Filter "TestRuntimeSmoke.txt" -File -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

$SmokeArguments = @(
    $Map,
    "-game",
    "-windowed",
    "-ResX=1280",
    "-ResY=720",
    "-nosound",
    "-unattended",
    "-NoSplash",
    "-RenderOffscreen",
    "-log",
    "-stdout",
    "-DEFAULTMAP=$Map",
    "-ndgiSmoke",
    "-ndgiSmokeTimeA=$TimeA",
    "-ndgiSmokeTimeB=$TimeB",
    "-ndgiSmokeMap=$Map"
)

$SmokeResult = Invoke-NDGIBatch -Executable $Executable.FullName -Arguments $SmokeArguments `
    -LogPath $SmokeLog -WorkingDirectory (Split-Path -Parent $Executable.FullName) `
    -TimeoutMinutes $SmokeTimeoutMinutes

$StateFile = if (Test-Path -LiteralPath $LocalStatePath) {
    Get-Item -LiteralPath $LocalStatePath
} else {
    Get-ChildItem -LiteralPath $ArchiveDirectory -Recurse -Filter "TestRuntimeSmoke.txt" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
$SmokeState = if ($StateFile) { (Get-Content -LiteralPath $StateFile.FullName -Raw).Trim() } else { "" }
if ($StateFile) {
    Write-Host "Runtime smoke state ($($StateFile.FullName)): $SmokeState"
}

if ($SmokeResult.TimedOut) {
    throw "Packaged smoke test timed out after $SmokeTimeoutMinutes minutes (last state: $SmokeState). See $SmokeLog"
}

$Passed = Get-NDGILogMatch -Path $SmokeLog -Pattern "TestRuntimeSmoke: PASSED" -Tail 1
$Failed = Get-NDGILogMatch -Path $SmokeLog -Pattern "TestRuntimeSmoke: FAILED|Fatal error|Assertion failed|LowLevelFatalError" -Tail 8 -CaseSensitive
$StatePassed = $SmokeState -match "\bPASSED\b"
$StateFailed = $SmokeState -match "\bFAILED\b"
if ((-not $StatePassed -and $Passed.Count -eq 0) -or $StateFailed -or $Failed.Count -gt 0) {
    foreach ($Line in $Failed) {
        Write-Host $Line.Line
    }
    throw "Packaged smoke test failed (state: $SmokeState). See $SmokeLog"
}

foreach ($Line in $Passed) {
    Write-Host $Line.Line
}
Write-Host ""
Write-Host "PACKAGED SMOKE PASSED"
Write-Host "Archive: $ArchiveDirectory"
}
finally {
    for ($Index = $QuarantineMoved.Count - 1; $Index -ge 0; --$Index) {
        $Item = $QuarantineMoved[$Index]
        if (Test-Path -LiteralPath $Item.Destination) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Item.Source) | Out-Null
            Move-Item -LiteralPath $Item.Destination -Destination $Item.Source -Force
        }
    }
    if ($QuarantineMoved.Count -gt 0) {
        Write-Host "Restored quarantined content."
    }
}
