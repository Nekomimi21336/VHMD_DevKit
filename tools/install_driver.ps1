<#
.SYNOPSIS
  Registers the VirtualHMD driver folder with SteamVR via vrpathreg.

.DESCRIPTION
  Uses SteamVR's own vrpathreg tool ("adddriver"), which adds the folder to
  external_drivers in %LOCALAPPDATA%\openvr\openvrpaths.vrpath. Nothing is
  copied into the SteamVR installation and no system settings are changed.
  Undo with tools/uninstall_driver.ps1.

.PARAMETER DriverPath
  Folder containing driver.vrdrivermanifest. Default: build\driver\vhmd
#>
param(
    [string]$DriverPath
)

. "$PSScriptRoot\common.ps1"

if (-not $DriverPath) { $DriverPath = Get-DefaultDriverPath }
$DriverPath = [System.IO.Path]::GetFullPath($DriverPath)

$manifest = Join-Path $DriverPath 'driver.vrdrivermanifest'
$dll = Join-Path $DriverPath 'bin\win64\driver_vhmd.dll'
if (-not (Test-Path $manifest) -or -not (Test-Path $dll)) {
    throw "Driver not built at '$DriverPath'. Run: cmake -S . -B build -A x64; cmake --build build --config Release"
}

if (Test-SteamVRRunning) {
    Write-Warning 'SteamVR is running. The driver is loaded on the next SteamVR start.'
}

$vrpathreg = Get-VrPathReg
$existing = Get-RegisteredExternalDrivers | Where-Object { $_ -and ([System.IO.Path]::GetFullPath($_) -ieq $DriverPath) }
if ($existing) {
    Write-Host "Already registered: $DriverPath"
} else {
    # Warn about other folders that also provide a driver named "vhmd".
    foreach ($d in Get-RegisteredExternalDrivers) {
        $m = Join-Path $d 'driver.vrdrivermanifest'
        if ((Test-Path $m) -and ((Get-Content $m -Raw) -match '"name"\s*:\s*"vhmd"')) {
            Write-Warning "Another vhmd driver folder is registered: $d (remove it with uninstall_driver.ps1 -DriverPath)"
        }
    }
    & $vrpathreg adddriver $DriverPath
    if ($LASTEXITCODE -ne 0) { throw "vrpathreg adddriver failed ($LASTEXITCODE)" }
    Write-Host "Registered: $DriverPath"
}

Write-Host ''
Write-Host 'Current SteamVR paths:'
& $vrpathreg show
