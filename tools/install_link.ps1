<#
.SYNOPSIS
  Sets up VHMD Link on this PC: registers the VHMD driver with SteamVR, adds
  "VHMD Link" to the Start menu, and (optionally) installs the headset app.

.DESCRIPTION
  1. Registers the driver (tools/install_driver.ps1, with its record of the
     SteamVR setup for undoing).
  2. Creates a Start menu shortcut (and with -Desktop a desktop shortcut) to
     "VHMD Link.exe" (the release's "VHMD Link" folder, or dist\VHMD Link after
     tools\build_link_exe.py): no Python needed. Without the exe it uses Python
     (pythonw clients\python\vhmd_link.pyw, Python 3.9+ with tkinter).
  3. With a headset connected over USB (developer mode, adb on PATH), installs
     the headset app: -Apk link (default: "VHMD Link"), dev ("VHMD Link Dev"),
     both, or none.
  Nothing outside your user profile is changed. Windows' firewall may ask once
  whether VHMD Link (python) may use private networks: allow it for Wi-Fi.
  Undo with tools/uninstall_link.ps1.

.PARAMETER Apk
  Headset app to install over USB: link, dev, both or none.

.PARAMETER Desktop
  Also put a shortcut on the desktop.

.PARAMETER Start
  Open VHMD Link when done.
#>
param(
    [ValidateSet('link', 'dev', 'both', 'none')][string]$Apk = 'link',
    [switch]$Desktop,
    [switch]$Start
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# 1. The driver.
& "$PSScriptRoot\install_driver.ps1"

# 2. The app: "VHMD Link.exe" if there is one, else Python.
$exe = @((Join-Path $root 'VHMD Link\VHMD Link.exe'), (Join-Path $root 'dist\VHMD Link\VHMD Link.exe')) |
       Where-Object { Test-Path $_ } | Select-Object -First 1
$python = $null
foreach ($cand in @('pythonw.exe', 'python.exe')) {
    $cmd = Get-Command $cand -ErrorAction SilentlyContinue
    if ($cmd) { $python = $cmd.Source; break }
}
if ($exe) {
    $target = $exe
    $arguments = ''
    $workdir = Split-Path $exe
    $icon = "$exe,0"
    Write-Host "App: $exe"
} else {
    if (-not $python) {
        $py = Get-Command 'py.exe' -ErrorAction SilentlyContinue
        if ($py) { $python = (& $py.Source -3 -c "import sys; print(sys.executable)").Trim() }
    }
    if (-not $python) {
        throw 'No "VHMD Link.exe" here, and no Python 3.9+ (https://www.python.org/downloads/windows/).'
    }
    $pythonw = Join-Path (Split-Path $python) 'pythonw.exe'
    if (-not (Test-Path $pythonw)) { $pythonw = $python }
    $console = Join-Path (Split-Path $python) 'python.exe'
    & $console -c "import sys, tkinter; sys.exit(0 if sys.version_info >= (3, 9) else 1)"
    if ($LASTEXITCODE -ne 0) { throw "VHMD Link needs Python 3.9+ with tkinter ($console)" }
    $script = Join-Path $root 'clients\python\vhmd_link.pyw'
    $workdir = Join-Path $root 'clients\python'
    $target = $pythonw
    $arguments = "`"$script`""
    # The app icon, drawn by the app (needs Pillow; Windows' default icon otherwise).
    $icon = (& $console -c "import sys; sys.path.insert(0, r'$workdir'); from vhmd_client.link import make_icon; print(make_icon() or '')").Trim()
    Write-Host "App: Python ($pythonw)"
}

$shell = New-Object -ComObject WScript.Shell
$places = @([Environment]::GetFolderPath('Programs'))
if ($Desktop) { $places += [Environment]::GetFolderPath('Desktop') }
foreach ($dir in $places) {
    $lnk = $shell.CreateShortcut((Join-Path $dir 'VHMD Link.lnk'))
    $lnk.TargetPath = $target
    $lnk.Arguments = $arguments
    $lnk.WorkingDirectory = $workdir
    $lnk.Description = 'VHMD Link: connect a standalone headset to SteamVR'
    if ($icon) { $lnk.IconLocation = $icon }
    $lnk.Save()
    Write-Host "Shortcut: $(Join-Path $dir 'VHMD Link.lnk')"
}

# 3. The headset app.
if ($Apk -ne 'none') {
    $apks = @()
    foreach ($name in @('vhmd-link.apk', 'vhmd-link-dev.apk')) {
        $wanted = ($Apk -eq 'both') -or ($Apk -eq 'link' -and $name -eq 'vhmd-link.apk') -or
                  ($Apk -eq 'dev' -and $name -eq 'vhmd-link-dev.apk')
        if (-not $wanted) { continue }
        $p = @((Join-Path $root "headset\$name"), (Join-Path $root "headset\android\build\$name")) |
             Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($p) { $apks += $p } else { Write-Warning "$name not found (build it with headset\android\build_apk.py)" }
    }
    $adb = Get-Command 'adb.exe' -ErrorAction SilentlyContinue
    $device = $false
    if ($adb -and $apks) {
        $device = (& $adb.Source devices) -match "`tdevice$"
    }
    if (-not $apks) {
    } elseif (-not $adb) {
        Write-Host 'adb not found: install the headset app yourself (see headset\README.md).'
    } elseif (-not $device) {
        Write-Host 'No headset on USB (developer mode, USB debugging allowed): the headset app was not installed.'
    } else {
        foreach ($p in $apks) {
            Write-Host "Installing $(Split-Path $p -Leaf) on the headset..."
            & $adb.Source install -r -g $p
            if ($LASTEXITCODE -ne 0) { Write-Warning "adb install failed ($LASTEXITCODE)" }
        }
    }
}

Write-Host ''
Write-Host 'VHMD Link is set up. Open it from the Start menu ("VHMD Link"), then start VHMD Link on the headset.'
if ($Start) {
    if ($arguments) { Start-Process -FilePath $target -ArgumentList $arguments -WorkingDirectory $workdir }
    else { Start-Process -FilePath $target -WorkingDirectory $workdir }
}
