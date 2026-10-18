# Shared helpers for the VirtualHMD tool scripts (dot-source this file).
# Read-only discovery of Steam / SteamVR locations; nothing here modifies the system.

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
    return Join-Path $script:RepoRoot 'build\driver\vhmd'
}
