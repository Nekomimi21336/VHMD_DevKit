# Shared helpers for the VirtualHMD tool scripts (dot-source this file).
# Discovery of Steam / SteamVR locations, plus the install records kept under
# %LOCALAPPDATA%\VHMD_DevKit\history (see Save-VhmdRecord).

$ErrorActionPreference = 'Stop'

$script:RepoRoot = Split-Path -Parent $PSScriptRoot

function Get-SteamPath {
    $p = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if (-not $p) {
        $p = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
    }
    if ($p) { return [System.IO.Path]::GetFullPath($p) }
    return $null
}

function Get-SteamLibraryFolders([string]$SteamPath) {
    $vdf = Join-Path $SteamPath 'steamapps\libraryfolders.vdf'
    $folders = @($SteamPath)
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $folders += ($m.Groups[1].Value -replace '\\\\', '\')
        }
    }
    return $folders | Select-Object -Unique
}

function Find-SteamApp([string]$SteamPath, [string]$AppId) {
    foreach ($lib in Get-SteamLibraryFolders $SteamPath) {
        $acf = Join-Path $lib "steamapps\appmanifest_$AppId.acf"
        if (Test-Path $acf) {
            $text = Get-Content $acf -Raw
            $dir = [regex]::Match($text, '"installdir"\s+"([^"]+)"').Groups[1].Value
            return [pscustomobject]@{
                AppId   = $AppId
                Path    = Join-Path $lib "steamapps\common\$dir"
                BuildId = [regex]::Match($text, '"buildid"\s+"([^"]+)"').Groups[1].Value
            }
        }
    }
    return $null
}

function Get-SteamVRPath {
    # Prefer the runtime path SteamVR itself registered.
    $vrpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (Test-Path $vrpath) {
        try {
            $j = Get-Content $vrpath -Raw | ConvertFrom-Json
            foreach ($r in $j.runtime) { if (Test-Path (Join-Path $r 'bin\win64\vrpathreg.exe')) { return $r } }
        } catch { }
    }
    $steam = Get-SteamPath
    if ($steam) {
        $app = Find-SteamApp $steam '250820'
        if ($app -and (Test-Path $app.Path)) { return $app.Path }
    }
    return $null
}

function Get-VrPathReg {
    $vr = Get-SteamVRPath
    if (-not $vr) { throw 'SteamVR installation not found.' }
    $exe = Join-Path $vr 'bin\win64\vrpathreg.exe'
    if (-not (Test-Path $exe)) { throw "vrpathreg.exe not found at $exe" }
    return $exe
}

function Get-RegisteredExternalDrivers {
    $vrpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (-not (Test-Path $vrpath)) { return @() }
    $j = Get-Content $vrpath -Raw | ConvertFrom-Json
    return @($j.external_drivers | Where-Object { $_ })
}

function Test-SteamVRRunning {
    return [bool](Get-Process -Name vrserver, vrmonitor, vrcompositor -ErrorAction SilentlyContinue)
}

function Get-DefaultDriverPath {
    # Release archives ship the driver as <root>\vhmd; source checkouts build it.
    $packaged = Join-Path $script:RepoRoot 'vhmd'
    if (Test-Path (Join-Path $packaged 'driver.vrdrivermanifest')) { return $packaged }
    return Join-Path $script:RepoRoot 'build\driver\vhmd'
}

# ---- Install records --------------------------------------------------------
# install_driver.ps1 saves the SteamVR files the driver can affect before it
# changes anything; uninstall_driver.ps1 uses the record to clean up or to put
# the files back. Records are never deleted by the scripts.

$script:VhmdSettingsSections = @('driver_vhmd', 'vhmd_display')

function Get-OpenVRPathsFile { return Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath' }

function Get-SteamVRConfigDir {
    $f = Get-OpenVRPathsFile
    if (Test-Path $f) {
        try {
            $c = @((Get-Content $f -Raw | ConvertFrom-Json).config)[0]
            if ($c -and (Test-Path $c)) { return [System.IO.Path]::GetFullPath($c) }
        } catch { }
    }
    $steam = Get-SteamPath
    if ($steam) { return Join-Path $steam 'config' }
    return $null
}

function Get-RecordedFiles {
    # Name inside a record -> current location.
    $files = [ordered]@{ 'openvrpaths.vrpath' = (Get-OpenVRPathsFile) }
    $cfg = Get-SteamVRConfigDir
    if ($cfg) {
        $files['steamvr.vrsettings'] = Join-Path $cfg 'steamvr.vrsettings'
        $files['chaperone_info.vrchap'] = Join-Path $cfg 'chaperone_info.vrchap'
    }
    return $files
}

function Get-VhmdHistoryDir { return Join-Path $env:LOCALAPPDATA 'VHMD_DevKit\history' }

function Get-VhmdVersion {
    $cl = Join-Path $script:RepoRoot 'CHANGELOG.md'
    if (Test-Path $cl) {
        $m = [regex]::Match((Get-Content $cl -Raw -Encoding UTF8), '(?m)^## v([0-9][0-9.]*)')
        if ($m.Success) { return $m.Groups[1].Value }
    }
    return 'unknown'
}

function Write-VhmdJson([string]$Path, $Object) {
    $text = $Object | ConvertTo-Json -Depth 16
    [System.IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# Copies the SteamVR files and writes record.json; returns the record folder.
function Save-VhmdRecord([string]$Kind, [string]$DriverPath) {
    $history = Get-VhmdHistoryDir
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dir = Join-Path $history "$stamp-$Kind"
    for ($n = 2; Test-Path $dir; $n++) { $dir = Join-Path $history "$stamp-$Kind-$n" }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    $files = [ordered]@{}
    foreach ($e in (Get-RecordedFiles).GetEnumerator()) {
        $info = [ordered]@{ path = $e.Value; existed = [bool](Test-Path -LiteralPath $e.Value) }
        if ($info.existed) {
            Copy-Item -LiteralPath $e.Value -Destination (Join-Path $dir $e.Key)
            $info.sha256 = (Get-FileHash -LiteralPath $e.Value -Algorithm SHA256).Hash
        }
        $files[$e.Key] = $info
    }
    $build = $null
    $steam = Get-SteamPath
    if ($steam) { $app = Find-SteamApp $steam '250820'; if ($app) { $build = $app.BuildId } }
    $record = [ordered]@{
        kind             = $Kind
        created          = (Get-Date).ToString('o')
        vhmd_version     = Get-VhmdVersion
        driver_path      = $DriverPath
        steamvr_path     = Get-SteamVRPath
        steamvr_buildid  = $build
        steamvr_running  = Test-SteamVRRunning
        external_drivers = @(Get-RegisteredExternalDrivers)
        files            = $files
    }
    if ($DriverPath -and (Test-Path -LiteralPath $DriverPath)) {
        $root = $DriverPath.TrimEnd('\').Length + 1
        $list = Get-ChildItem -LiteralPath $DriverPath -Recurse -File | ForEach-Object { $_.FullName.Substring($root) }
        [System.IO.File]::WriteAllLines((Join-Path $dir 'driver_files.txt'), [string[]]@($list))
    }
    Write-VhmdJson (Join-Path $dir 'record.json') $record
    return $dir
}

function Read-VhmdRecord([string]$Dir) {
    $f = Join-Path $Dir 'record.json'
    if (-not (Test-Path -LiteralPath $f)) { throw "No record.json in $Dir" }
    $r = Get-Content -LiteralPath $f -Raw -Encoding UTF8 | ConvertFrom-Json
    $r | Add-Member -NotePropertyName dir -NotePropertyValue ([System.IO.Path]::GetFullPath($Dir)) -Force
    return $r
}

# All records, oldest first.
function Get-VhmdRecords {
    $history = Get-VhmdHistoryDir
    if (-not (Test-Path $history)) { return }
    foreach ($d in Get-ChildItem -LiteralPath $history -Directory | Sort-Object Name) {
        if (Test-Path -LiteralPath (Join-Path $d.FullName 'record.json')) { Read-VhmdRecord $d.FullName }
    }
}

function Set-VhmdRecordField($Record, [string]$Name, $Value) {
    $Record | Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force
    Write-VhmdJson (Join-Path $Record.dir 'record.json') ($Record | Select-Object -Property * -ExcludeProperty dir)
}

function Test-VhmdSettingsKey([string]$Section, [string]$Key) {
    if ($script:VhmdSettingsSections -contains $Section) { return $true }
    return ($Section -eq 'trackers' -and $Key -like '/devices/vhmd/*')
}

# Removes the driver's own entries from steamvr.vrsettings; returns $true if
# the file changed.
function Remove-VhmdSettings([string]$File) {
    if (-not (Test-Path -LiteralPath $File)) { return $false }
    $json = Get-Content -LiteralPath $File -Raw -Encoding UTF8 | ConvertFrom-Json
    $changed = $false
    foreach ($section in $script:VhmdSettingsSections) {
        if ($json.PSObject.Properties[$section]) { $json.PSObject.Properties.Remove($section); $changed = $true }
    }
    if ($json.PSObject.Properties['trackers']) {
        foreach ($p in @($json.trackers.PSObject.Properties)) {
            if (Test-VhmdSettingsKey 'trackers' $p.Name) { $json.trackers.PSObject.Properties.Remove($p.Name); $changed = $true }
        }
    }
    if ($changed) { Write-VhmdJson $File $json }
    return $changed
}

# Lists settings that differ between two steamvr.vrsettings files, ignoring
# the driver's own entries: "+" added, "-" removed, "~" changed.
function Compare-VrSettings([string]$BeforeFile, [string]$AfterFile) {
    $before = New-Object psobject
    $after = New-Object psobject
    if (Test-Path -LiteralPath $BeforeFile) { $before = Get-Content -LiteralPath $BeforeFile -Raw -Encoding UTF8 | ConvertFrom-Json }
    if (Test-Path -LiteralPath $AfterFile) { $after = Get-Content -LiteralPath $AfterFile -Raw -Encoding UTF8 | ConvertFrom-Json }
    $out = @()
    $sections = @($before.PSObject.Properties.Name) + @($after.PSObject.Properties.Name) | Select-Object -Unique
    foreach ($s in $sections) {
        if ($script:VhmdSettingsSections -contains $s) { continue }
        $bs = $before.PSObject.Properties[$s]; $as = $after.PSObject.Properties[$s]
        if (-not $bs) { $out += "+ $s"; continue }
        if (-not $as) { $out += "- $s"; continue }
        if (($bs.Value -is [System.Management.Automation.PSCustomObject]) -and ($as.Value -is [System.Management.Automation.PSCustomObject])) {
            $keys = @($bs.Value.PSObject.Properties.Name) + @($as.Value.PSObject.Properties.Name) | Select-Object -Unique
            foreach ($k in $keys) {
                if (Test-VhmdSettingsKey $s $k) { continue }
                $bk = $bs.Value.PSObject.Properties[$k]; $ak = $as.Value.PSObject.Properties[$k]
                if (-not $bk) { $out += "+ $s.$k" }
                elseif (-not $ak) { $out += "- $s.$k" }
                elseif ((ConvertTo-Json $bk.Value -Compress -Depth 16) -ne (ConvertTo-Json $ak.Value -Compress -Depth 16)) { $out += "~ $s.$k" }
            }
        } elseif ((ConvertTo-Json $bs.Value -Compress -Depth 16) -ne (ConvertTo-Json $as.Value -Compress -Depth 16)) {
            $out += "~ $s"
        }
    }
    return $out
}

# Files SteamVR or the driver create inside the driver folder at run time:
# labelled render models, label icons and SteamVR's hashed icon copies.
function Remove-VhmdGeneratedFiles([string]$DriverPath) {
    $res = Join-Path $DriverPath 'resources'
    if (-not (Test-Path -LiteralPath $res)) { return 0 }
    $items = @()
    $labels = Join-Path $res 'icons\labels'
    if (Test-Path -LiteralPath $labels) { $items += Get-Item -LiteralPath $labels }
    $models = Join-Path $res 'rendermodels'
    if (Test-Path -LiteralPath $models) { $items += @(Get-ChildItem -LiteralPath $models -Directory -Filter 'vhmd_vhmd-*') }
    $icons = Join-Path $res 'icons'
    if (Test-Path -LiteralPath $icons) {
        $items += @(Get-ChildItem -LiteralPath $icons -File | Where-Object { $_.Name -match '\.[0-9a-f]{8}\.(png|gif)$' })
    }
    foreach ($i in $items) { Remove-Item -LiteralPath $i.FullName -Recurse -Force }
    return $items.Count
}
