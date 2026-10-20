<#
.SYNOPSIS
  Unregisters the VirtualHMD driver from SteamVR and cleans up after it.

.DESCRIPTION
  First records the current SteamVR files (like install_driver.ps1, as
  %LOCALAPPDATA%\VHMD_DevKit\history\<time>-uninstall), so this step can be
  undone too. Then, by default:
    - unregisters every registered folder whose manifest names the driver
      "vhmd" (vrpathreg removedriver),
    - deletes the files the driver and SteamVR generated inside those folders
      (labelled render models, label icons, SteamVR's hashed icon copies),
    - removes the driver's entries from steamvr.vrsettings ("driver_vhmd",
      "vhmd_display", tracker roles under /devices/vhmd/),
    - lists what else changed in SteamVR's settings and driver list since the
      install record, without touching it.
  With -Restore, registrations, steamvr.vrsettings and chaperone_info.vrchap
  are put back exactly as recorded at install time instead.

.PARAMETER DriverPath
  Only unregister this folder.

.PARAMETER Restore
  Put the recorded files and driver registrations back (requires SteamVR to
  be closed). Settings changed after the install are lost, but the uninstall
  record keeps them.

.PARAMETER Record
  Record folder to compare with or restore from. Default: the oldest install
  record not yet followed by an uninstall. Pass an uninstall record with
  -Restore to undo an uninstall.

.PARAMETER KeepSettings
  Leave steamvr.vrsettings as it is.

.PARAMETER KeepGeneratedFiles
  Leave the generated files in the driver folder.

.PARAMETER List
  Show the records and exit.

.PARAMETER CleanSettings
  Accepted for compatibility; cleaning the settings is now the default.
#>
param(
    [string]$DriverPath,
    [switch]$Restore,
    [string]$Record,
    [switch]$KeepSettings,
    [switch]$KeepGeneratedFiles,
    [switch]$List,
    [switch]$CleanSettings
)

. "$PSScriptRoot\common.ps1"

if ($List) {
    $records = @(Get-VhmdRecords)
    if (-not $records) { Write-Host "No records in $(Get-VhmdHistoryDir)"; return }
    $records | ForEach-Object {
        [pscustomobject]@{
            Record  = Split-Path -Leaf $_.dir
            Version = $_.vhmd_version
            Closed  = if ($_.PSObject.Properties['closed']) { ([datetime]$_.closed).ToString('yyyy-MM-dd HH:mm') } else { '' }
            Driver  = $_.driver_path
        }
    } | Format-Table -AutoSize
    return
}

function Test-InList([string]$Item, $List) { return [bool]($List | Where-Object { $_ -ieq $Item }) }

$vrpathreg = Get-VrPathReg
if ($Record) {
    $ref = Read-VhmdRecord $Record
} else {
    $ref = Get-VhmdRecords | Where-Object { $_.kind -eq 'install' -and -not $_.PSObject.Properties['closed'] } | Select-Object -First 1
}
$running = Test-SteamVRRunning
if ($Restore) {
    if (-not $ref) { throw "No install record found in $(Get-VhmdHistoryDir); nothing to restore." }
    if ($running) { throw 'Close SteamVR before using -Restore (SteamVR rewrites its settings on exit).' }
}
if ($ref) { Write-Host "Reference record: $($ref.dir)" } else { Write-Host 'No install record found; cleaning up without one.' }

$files = Get-RecordedFiles
$undo = Read-VhmdRecord (Save-VhmdRecord 'uninstall' $null)
Write-Host "Recorded the current SteamVR setup: $($undo.dir)"
if ($running) { Write-Warning 'SteamVR is running; registration changes apply after it restarts.' }

# Driver registrations.
$removed = @()
if ($Restore) {
    $want = @($ref.external_drivers | Where-Object { $_ })
    foreach ($d in Get-RegisteredExternalDrivers) {
        if (-not (Test-InList $d $want)) {
            & $vrpathreg removedriver $d
            if ($LASTEXITCODE -eq 0) { Write-Host "Unregistered: $d"; $removed += $d } else { Write-Warning "vrpathreg removedriver failed for $d ($LASTEXITCODE)" }
        }
    }
    foreach ($d in $want) {
        if (Test-InList $d @(Get-RegisteredExternalDrivers)) { continue }
        if (-not (Test-Path -LiteralPath $d)) { Write-Warning "Recorded driver folder no longer exists, not registered again: $d"; continue }
        & $vrpathreg adddriver $d
        if ($LASTEXITCODE -eq 0) { Write-Host "Registered again: $d" } else { Write-Warning "vrpathreg adddriver failed for $d ($LASTEXITCODE)" }
    }
} else {
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
    if (-not $targets) { Write-Host 'No registered vhmd driver folder found.' }
    foreach ($t in $targets) {
        & $vrpathreg removedriver $t
        if ($LASTEXITCODE -ne 0) { Write-Warning "vrpathreg removedriver failed for $t ($LASTEXITCODE)" }
        else { Write-Host "Unregistered: $t"; $removed += $t }
    }
}

# Files generated inside the unregistered vhmd folders.
if (-not $KeepGeneratedFiles) {
    foreach ($t in $removed) {
        $m = Join-Path $t 'driver.vrdrivermanifest'
        if (-not ((Test-Path $m) -and ((Get-Content $m -Raw) -match '"name"\s*:\s*"vhmd"'))) { continue }
        $n = Remove-VhmdGeneratedFiles $t
        if ($n) { Write-Host "Deleted $n generated item(s) in $t\resources" }
    }
}

# SteamVR settings.
$settings = $files['steamvr.vrsettings']
if ($Restore) {
    foreach ($name in 'steamvr.vrsettings', 'chaperone_info.vrchap') {
        $info = $ref.files.$name
        $dest = $files[$name]
        if (-not $info -or -not $dest) { continue }
        if ($info.existed) {
            Copy-Item -LiteralPath (Join-Path $ref.dir $name) -Destination $dest -Force
            Write-Host "Restored: $dest"
        } elseif (Test-Path -LiteralPath $dest) {
            Remove-Item -LiteralPath $dest
            Write-Host "Removed (did not exist when recorded): $dest"
        }
    }
    if ($ref.steamvr_running) { Write-Warning 'SteamVR was running when that record was made; its settings file may not have been final.' }
} elseif (-not $KeepSettings) {
    if ($running) {
        Write-Warning 'SteamVR is running; the vhmd entries in steamvr.vrsettings were left. Close SteamVR and run this again.'
    } elseif (Remove-VhmdSettings $settings) {
        Write-Host "Removed the vhmd entries from $settings"
    } else {
        Write-Host 'No vhmd entries in steamvr.vrsettings.'
    }
}

# What else differs from the record (reported only).
if ($ref -and -not $Restore) {
    $diff = @(Compare-VrSettings (Join-Path $ref.dir 'steamvr.vrsettings') $settings)
    $now = @(Get-RegisteredExternalDrivers)
    $was = @($ref.external_drivers | Where-Object { $_ })
    foreach ($d in $now) { if (-not (Test-InList $d $was)) { $diff += "+ driver $d" } }
    foreach ($d in $was) { if (-not (Test-InList $d $now) -and -not (Test-InList $d $removed)) { $diff += "- driver $d" } }
    Write-Host ''
    if ($diff) {
        Write-Host "Differences from the record (left as they are; -Restore puts the recorded state back):"
        $diff | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host 'Everything else matches the record.'
    }
}

# Close the install records this uninstall answers.
$stamp = (Get-Date).ToString('o')
foreach ($r in @(Get-VhmdRecords | Where-Object { $_.kind -eq 'install' -and -not $_.PSObject.Properties['closed'] })) {
    if ($Restore -or (Test-InList $r.driver_path $removed)) { Set-VhmdRecordField $r 'closed' $stamp }
}

Write-Host ''
& $vrpathreg show
