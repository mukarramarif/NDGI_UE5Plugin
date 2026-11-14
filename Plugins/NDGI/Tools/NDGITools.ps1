# Shared helpers for the NDGI packaging and CI scripts.
# Windows-only: UnrealEditor-Cmd.exe / RunUAT.bat and the cooked DX12 target.

$ErrorActionPreference = "Stop"

function Get-NDGIProjectRoot {
    param([string]$Project = "")

    if ($Project) {
        return (Split-Path -Parent (Resolve-Path -LiteralPath $Project).Path)
    }
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
}

function Resolve-NDGIProject {
    param([string]$Project = "")

    if (-not $Project) {
        $Project = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\RPG_Prototype.uproject")).Path
    }
    if (-not (Test-Path -LiteralPath $Project)) {
        throw "Project not found: $Project"
    }
    return (Resolve-Path -LiteralPath $Project).Path
}

function ConvertTo-NDGIBatchArgument {
    param([Parameter(Mandatory = $true)][string]$Value)

    # Percent signs are expanded by cmd.exe even inside quotes, so escape them.
    $Escaped = $Value.Replace("%", "%%")
    if ($Escaped -match '[\s"&|<>^]') {
        return '"' + $Escaped + '"'
    }
    return $Escaped
}

function New-NDGITempDirectory {
    param([string]$Name = "NDGI")

    $Directory = Join-Path $env:TEMP $Name
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    return $Directory
}

<#
.SYNOPSIS
Runs a native tool through cmd.exe with stdout/stderr redirected to a log file.

.DESCRIPTION
UE tools can spawn detached children (Zen, CrashReportClient) that inherit
stdout. Capturing that output through a PowerShell pipeline can therefore hang
long after Unreal exits. This helper generates a small .cmd wrapper that
redirects the child process itself, then waits for cmd.exe to finish.
#>
function Invoke-NDGIBatch {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [string]$WorkingDirectory = "",
        [int]$TimeoutMinutes = 45
    )

    if (-not (Test-Path -LiteralPath $Executable)) {
        throw "Executable not found: $Executable"
    }
    if (-not $WorkingDirectory) {
        $WorkingDirectory = (Get-Location).Path
    }

    $LogDirectory = Split-Path -Parent $LogPath
    if ($LogDirectory) {
        New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
    }

    $TempDirectory = New-NDGITempDirectory
    $BatchPath = Join-Path $TempDirectory ("ndgi_" + [guid]::NewGuid().ToString("N") + ".cmd")
    $QuotedExecutable = ConvertTo-NDGIBatchArgument $Executable
    $QuotedLog = ConvertTo-NDGIBatchArgument $LogPath
    $QuotedWorkingDirectory = ConvertTo-NDGIBatchArgument $WorkingDirectory
    $ArgumentText = (($Arguments | ForEach-Object { ConvertTo-NDGIBatchArgument $_ }) -join " ")

    $BatchLines = @(
        "@echo off",
        "setlocal",
        "chcp 65001 >nul",
        "cd /d $QuotedWorkingDirectory",
        "$QuotedExecutable $ArgumentText 1> $QuotedLog 2>&1",
        "exit /b %ERRORLEVEL%"
    )
    Set-Content -LiteralPath $BatchPath -Value $BatchLines -Encoding ASCII

    $StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $StartInfo.FileName = $env:ComSpec
    $StartInfo.Arguments = "/d /c `"$BatchPath`""
    $StartInfo.WorkingDirectory = $WorkingDirectory
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true

    $Process = [System.Diagnostics.Process]::Start($StartInfo)
    $TimeoutMs = [Math]::Max(1, $TimeoutMinutes) * 60 * 1000
    if (-not $Process.WaitForExit($TimeoutMs)) {
        try {
            & taskkill.exe /PID $Process.Id /T /F 2>$null | Out-Null
        } catch {
            try { $Process.Kill() } catch { }
        }
        return [pscustomobject]@{
            ExitCode = -1
            TimedOut = $true
            LogPath = $LogPath
            BatchPath = $BatchPath
        }
    }

    return [pscustomobject]@{
        ExitCode = $Process.ExitCode
        TimedOut = $false
        LogPath = $LogPath
        BatchPath = $BatchPath
    }
}

function Invoke-NDGIUnreal {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$EditorCmd,
        [Parameter(Mandatory = $true)][string]$Project,
        [Parameter(Mandatory = $true)][string]$Map,
        [Parameter(Mandatory = $true)][string]$ExecCmds,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [switch]$Game,
        [int]$ResX = 1280,
        [int]$ResY = 720,
        [int]$TimeoutMinutes = 30
    )

    $Arguments = @($Project, $Map)
    if ($Game) {
        $Arguments += @("-game", "-windowed", "-ResX=$ResX", "-ResY=$ResY")
    }
    $Arguments += @(
        "-nosound",
        "-unattended",
        "-NoSplash",
        "-RenderOffscreen",
        "-log",
        "-stdout",
        "-ExecCmds=$ExecCmds"
    )

    return Invoke-NDGIBatch -Executable $EditorCmd -Arguments $Arguments -LogPath $LogPath `
        -WorkingDirectory (Split-Path -Parent $Project) -TimeoutMinutes $TimeoutMinutes
}

function Get-NDGILogMatch {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [int]$Tail = 1,
        [switch]$CaseSensitive
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return @()
    }
    $Params = @{
        Path = $Path
        Pattern = $Pattern
        AllMatches = $true
    }
    if ($CaseSensitive) {
        $Params.CaseSensitive = $true
    }
    return @(Select-String @Params | Select-Object -Last $Tail)
}

function Get-NDGILogNumber {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [int]$Group = 1,
        [double]$Default = [double]::NaN
    )

    $Matches = Get-NDGILogMatch -Path $Path -Pattern $Pattern
    if ($Matches.Count -eq 0) {
        return $Default
    }
    $Match = $Matches[-1].Matches | Select-Object -Last 1
    if (-not $Match) {
        return $Default
    }
    $Text = $Match.Groups[$Group].Value
    $Number = 0.0
    if ([double]::TryParse($Text, [ref]$Number)) {
        return $Number
    }
    return $Default
}
