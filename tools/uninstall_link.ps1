<#
.SYNOPSIS
  Removes what install_link.ps1 set up: the VHMD Link shortcuts and the
  "open when Windows starts" entry, then unregisters the driver
  (tools/uninstall_driver.ps1; extra arguments such as -Restore go to it).

.DESCRIPTION
  VHMD Link's own settings in %LOCALAPPDATA%\VHMD are kept (-Purge removes them;
  the VHMD config profiles there, which SteamVR may point at, always stay).
  The headset app is removed on the headset (or: adb uninstall com.vhmd.link).

.PARAMETER Purge
  Also delete VHMD Link's settings and icon in %LOCALAPPDATA%\VHMD.

.PARAMETER KeepDriver
  Only remove VHMD Link; leave the driver registered.
#>
param(
    [switch]$Purge,
    [switch]$KeepDriver,
    [Parameter(ValueFromRemainingArguments = $true)]$DriverArgs
)

$ErrorActionPreference = 'Stop'

Get-Process 'VHMD Link' -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "Closing VHMD Link (process $($_.Id))"; Stop-Process -Id $_.Id }
Get-Process pythonw, python -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)").CommandLine -match 'vhmd_link\.pyw|vhmd_client\.link' } |
    ForEach-Object { Write-Host "Closing VHMD Link (process $($_.Id))"; Stop-Process -Id $_.Id }

foreach ($dir in @([Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('Desktop'))) {
    $lnk = Join-Path $dir 'VHMD Link.lnk'
    if (Test-Path $lnk) { Remove-Item $lnk; Write-Host "Removed $lnk" }
}
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if (Get-ItemProperty -Path $run -Name 'VHMD Link' -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $run -Name 'VHMD Link'
    Write-Host 'Removed the "open when Windows starts" entry'
}
if ($Purge) {
    foreach ($name in @('link_settings.json', 'vhmd_link.ico', 'vhmd_link.png')) {
        $f = Join-Path $env:LOCALAPPDATA "VHMD\$name"
        if (Test-Path $f) { Remove-Item $f; Write-Host "Removed $f" }
    }
}
if (-not $KeepDriver) {
    & "$PSScriptRoot\uninstall_driver.ps1" @DriverArgs
}
