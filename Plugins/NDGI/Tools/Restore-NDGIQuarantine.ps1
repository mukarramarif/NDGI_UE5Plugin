param(
    [string]$Project = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "NDGITools.ps1")

$Project = Resolve-NDGIProject -Project $Project
$ProjectRoot = Split-Path -Parent $Project
$QuarantineRoot = Join-Path $ProjectRoot "Saved\NDGI\PackagingQuarantine"
if (-not (Test-Path -LiteralPath $QuarantineRoot)) {
    Write-Host "No NDGI packaging quarantine found."
    return
}

$Restored = 0
$QuarantineDirectories = Get-ChildItem -LiteralPath $QuarantineRoot -Directory | Sort-Object Name -Descending
foreach ($Directory in $QuarantineDirectories) {
    $Files = Get-ChildItem -LiteralPath $Directory.FullName -Recurse -File
    foreach ($File in $Files) {
        $Relative = $File.FullName.Substring($Directory.FullName.Length).TrimStart("\")
        $Destination = Join-Path $ProjectRoot $Relative
        if (Test-Path -LiteralPath $Destination) {
            Write-Warning "Skipping $Relative (destination already exists)."
            continue
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Move-Item -LiteralPath $File.FullName -Destination $Destination
        Write-Host "Restored $Relative"
        ++$Restored
    }
    if (-not (Get-ChildItem -LiteralPath $Directory.FullName -Recurse -File)) {
        Remove-Item -LiteralPath $Directory.FullName -Recurse -Force
    }
}

if ($Restored -eq 0) {
    Write-Host "No quarantined files needed restoring."
} else {
    Write-Host "Restored $Restored quarantined file(s)."
}
