<#
.SYNOPSIS
  Detects the development / test environment (read-only) and prints a report.

.PARAMETER Json
  Emit the report as JSON (for logs / CI).
#>
param([switch]$Json)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Continue'

function Get-ToolVersion([string]$cmd, [string[]]$cmdArgs) {
    $c = Get-Command $cmd -ErrorAction SilentlyContinue
    if (-not $c) { return $null }
    try { return ((& $c.Source @cmdArgs 2>&1) | Select-Object -First 1).ToString().Trim() } catch { return 'found' }
}

$report = [ordered]@{}
$steam = Get-SteamPath
$report.steam_path = $steam
$steamvr = Get-SteamVRPath
$report.steamvr_path = $steamvr
if ($steam) {
    $vrApp = Find-SteamApp $steam '250820'
    $report.steamvr_buildid = $vrApp.BuildId
    $vrc = Find-SteamApp $steam '438100'
    $report.vrchat_path = $vrc.Path
    $report.vrchat_buildid = $vrc.BuildId
    $log = Join-Path $steam 'logs\vrserver.txt'
    if (Test-Path $log) {
        $m = Select-String -Path $log -Pattern 'vrserver (\d+\.\d+\.\d+) startup' | Select-Object -Last 1
        if ($m) { $report.steamvr_version = $m.Matches[0].Groups[1].Value }
    }
}
if ($steamvr) {
    $report.vrpathreg = Join-Path $steamvr 'bin\win64\vrpathreg.exe'
    $report.steamvr_bin_version = (Get-Content (Join-Path $steamvr 'bin\version.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
}
$report.external_drivers = @(Get-RegisteredExternalDrivers)
$report.steamvr_running = Test-SteamVRRunning
$lastKnown = $null
if ($steam -and (Test-Path (Join-Path $steam 'config\steamvr.vrsettings'))) {
    try {
        $s = Get-Content (Join-Path $steam 'config\steamvr.vrsettings') -Raw | ConvertFrom-Json
        $lastKnown = $s.LastKnown.ActualHMDDriver
        $report.steamvr_forced_driver = $s.steamvr.forcedDriver
        $report.steamvr_activate_multiple_drivers = $s.steamvr.activateMultipleDrivers
    } catch { }
}
$report.last_hmd_driver = $lastKnown

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    # PowerShell 5 emits a JSON array as one object: unroll it explicitly.
    $instances = (& $vswhere -all -products * -format json | Out-String | ConvertFrom-Json)
    $report.visual_studio = @(foreach ($i in $instances) {
        "$($i.displayName) $($i.catalog.productDisplayVersion) @ $($i.installationPath)"
    })
}
$report.cmake = Get-ToolVersion 'cmake' @('--version')
$report.git = Get-ToolVersion 'git' @('--version')
$report.python = Get-ToolVersion 'python' @('--version')
$py = Get-Command python -ErrorAction SilentlyContinue
if ($py) {
    $report.pyopenvr = (& $py.Source -c "import openvr;print(openvr.__version__)" 2>$null)
}
$ovr = Join-Path $script:RepoRoot 'third_party\openvr'
if (Test-Path (Join-Path $ovr '.git')) {
    $report.openvr_sdk = (git -C $ovr describe --tags 2>$null)
    if (-not $report.openvr_sdk) { $report.openvr_sdk = (git -C $ovr rev-parse --short HEAD 2>$null) }
}
$dll = Join-Path (Get-DefaultDriverPath) 'bin\win64\driver_vhmd.dll'
$report.driver_built = Test-Path $dll
$report.driver_registered = [bool](Get-RegisteredExternalDrivers | Where-Object { $_ -and ([System.IO.Path]::GetFullPath($_) -ieq (Get-DefaultDriverPath)) })

if ($Json) {
    $report | ConvertTo-Json -Depth 4
} else {
    foreach ($k in $report.Keys) {
        $v = $report[$k]
        if ($v -is [array]) { Write-Host ("{0,-34}" -f $k); $v | ForEach-Object { Write-Host "    $_" } }
        else { Write-Host ("{0,-34} {1}" -f $k, $v) }
    }
}
