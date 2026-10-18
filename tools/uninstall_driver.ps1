<#
.SYNOPSIS
  Unregisters the VirtualHMD driver from SteamVR (vrpathreg removedriver).

.PARAMETER DriverPath
  Registered folder to remove. Default: every registered folder whose
  manifest names the driver "vhmd".

.PARAMETER CleanSettings
  Also remove what the driver may have left in Steam\config\steamvr.vrsettings:
  the "driver_vhmd" and "vhmd_display" sections and tracker role entries
  under "trackers" whose key starts with /devices/vhmd/. A timestamped backup
  of the file is written first. Requires SteamVR to be closed.
#>
param(
    [string]$DriverPath,
    [switch]$CleanSettings
)

. "$PSScriptRoot\common.ps1"

$vrpathreg = Get-VrPathReg
$targets = @()
if ($DriverPath) {
    $targets = @([System.IO.Path]::GetFullPath($DriverPath))
} else {
    foreach ($d in Get-RegisteredExternalDrivers) {
        $m = Join-Path $d 'driver.vrdrivermanifest'
        if ((Test-Path $m) -and ((Get-Content $m -Raw) -match '"name"\s*:\s*"vhmd"')) { $targets += $d }
        elseif (-not (Test-Path $m) -and ($d -match '\\vhmd\\?$')) { $targets += $d } # folder deleted after registering
    }
}

if (-not $targets) {
    Write-Host 'No registered vhmd driver folder found.'
} else {
    if (Test-SteamVRRunning) { Write-Warning 'SteamVR is running; the change applies after it restarts.' }
    foreach ($t in $targets) {
        & $vrpathreg removedriver $t
        if ($LASTEXITCODE -ne 0) { Write-Warning "vrpathreg removedriver failed for $t ($LASTEXITCODE)" }
        else { Write-Host "Unregistered: $t" }
    }
}

if ($CleanSettings) {
    if (Test-SteamVRRunning) { throw 'Close SteamVR before using -CleanSettings (SteamVR rewrites the file on exit).' }
    $steam = Get-SteamPath
    $file = Join-Path $steam 'config\steamvr.vrsettings'
    if (-not (Test-Path $file)) {
        Write-Host 'steamvr.vrsettings not found; nothing to clean.'
    } else {
        $json = Get-Content $file -Raw | ConvertFrom-Json
        $changed = $false
        foreach ($section in 'driver_vhmd', 'vhmd_display') {
            if ($json.PSObject.Properties[$section]) { $json.PSObject.Properties.Remove($section); $changed = $true }
        }
        if ($json.PSObject.Properties['trackers']) {
            foreach ($p in @($json.trackers.PSObject.Properties)) {
                if ($p.Name -like '/devices/vhmd/*') { $json.trackers.PSObject.Properties.Remove($p.Name); $changed = $true }
            }
        }
        if ($changed) {
            $backup = "$file.vhmd-backup-$(Get-Date -Format yyyyMMdd-HHmmss)"
            Copy-Item $file $backup
            $text = $json | ConvertTo-Json -Depth 32
            [System.IO.File]::WriteAllText($file, $text, (New-Object System.Text.UTF8Encoding($false)))
            Write-Host "Cleaned vhmd entries from $file (backup: $backup)"
        } else {
            Write-Host 'No vhmd entries in steamvr.vrsettings.'
        }
    }
}

Write-Host ''
& $vrpathreg show
