# switchpro_vjoy installer — makes Nintendo Switch Pro Controllers show up
# as normal, calibratable Windows joysticks (vJoy 1, vJoy 2, ...) including
# gyro/accelerometer on spare axes. No Secure Boot changes, no cost.
#
# What it does:
#   1. Installs vJoy driver (already Microsoft-attestation-signed) if missing
#   2. Configures vJoy devices 1..N (8 axes, 16 buttons, 1 hat)
#   3. Names them "vJoy 1".."vJoy N" so they are easy to tell apart
#   4. Installs HidHide and hides the physical pad's dead entry (optional)
#   5. Copies the feeder to %LOCALAPPDATA%\SwitchProBridge and autostarts it
#
# Usage:  install.ps1 [-InstallDir DIR] [-GameExe PATH] [-Devices N]
#                     [-WithHidHide] [-NoAutostart]
# HidHide is OPT-IN (-WithHidHide): it hides the physical pad's dead
# duplicate entry in joy.cpl but needs per-exe whitelisting, which can
# break game input when exe paths change. Without it everything still
# works - joy.cpl just shows the dead entry next to the vJoy devices.

param(
    [string]$InstallDir = "$env:LOCALAPPDATA\SwitchProBridge",
    [string]$GameExe   = "",
    [int]$Devices     = 2,
    [switch]$WithHidHide,
    [switch]$NoHidHide,
    [switch]$NoAutostart
)

$ErrorActionPreference = 'Stop'
$PkgDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Info($m)  { Write-Host "[+] $m" -ForegroundColor Green }
function Warn($m)  { Write-Host "[!] $m" -ForegroundColor Yellow }
function Fail($m)  { Write-Host "[x] $m" -ForegroundColor Red; exit 1 }

# ---------- self-elevate ----------
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Requesting administrator rights (needed for driver install)..."
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    foreach ($kv in $PSBoundParameters.GetEnumerator()) {
        $argList += "-$($kv.Key)"; if ($null -ne $kv.Value) { $argList += "`"$($kv.Value)`"" }
    }
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    exit
}

Write-Host ""
Write-Host "=== Switch Pro Controller -> Windows Joystick Bridge ===" -ForegroundColor Cyan
Write-Host ""

# ---------- 1. vJoy driver ----------
$vjoyCfg = 'C:\Program Files\vJoy\x64\vJoyConfig.exe'
$vjoyDll = 'C:\Program Files\vJoy\x64\vJoyInterface.dll'
if (-not (Test-Path $vjoyCfg)) {
    $setup = Join-Path $PkgDir 'drivers\vJoySetup.exe'
    if (-not (Test-Path $setup)) { Fail "vJoySetup.exe not found in $PkgDir\drivers" }
    Info "Installing vJoy driver (silent)..."
    $p = Start-Process $setup -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
    if ($p.ExitCode -notin 0,3010 -and -not (Test-Path $vjoyCfg)) {
        Warn "Silent install failed (code $($p.ExitCode)) - launching installer UI..."
        Start-Process $setup -Wait
    }
    if (-not (Test-Path $vjoyCfg)) { Fail "vJoy install did not complete" }
    Info "vJoy driver installed"
} else {
    Info "vJoy driver already installed"
}

# ---------- 2. configure vJoy devices ----------
for ($i = 1; $i -le $Devices; $i++) {
    & $vjoyCfg $i -f -a X Y Z Rx Ry Rz Sl0 Sl1 -b 16 -s 1 | Out-Null
}
Info "Configured $Devices vJoy device(s): 8 axes, 16 buttons, 1 hat"

# ---------- 3. name the vJoy devices ----------
# The vJoy PDO is the root-enumerated device with service 'vjoy'; its HID
# children are the per-device gamepad collections (HID\HIDCLASS&COLxx\...).
$pdo = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.Service -eq 'vjoy' } | Select-Object -First 1
if ($pdo) {
    $kids = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -like 'HID\*' -and $_.InstanceId -match 'COL'
    } | Where-Object {
        (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data -eq $pdo.InstanceId
    } | Sort-Object InstanceId
    $n = 0
    foreach ($k in $kids) {
        $n++
        $name = "vJoy $n"
        $regPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($k.InstanceId)"
        try {
            Set-ItemProperty -Path $regPath -Name FriendlyName -Value $name -ErrorAction Stop
            Info "Named $($k.InstanceId) -> $name"
        } catch { Warn "Could not name $($k.InstanceId): $($_.Exception.Message)" }
    }
    if ($n -eq 0) { Warn "No vJoy HID collections found yet - names may appear after reboot" }
} else {
    Warn "vJoy PDO not found - skipping device naming"
}

# ---------- 4. install feeder ----------
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item (Join-Path $PkgDir "bin\switchpro_vjoy.exe") $InstallDir -Force
# drop stale helper dlls from older package versions
Remove-Item (Join-Path $InstallDir 'SDL3.dll') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $InstallDir 'vJoyInterface.dll') -Force -ErrorAction SilentlyContinue
Info "Feeder installed to $InstallDir"

# hidden + visible launchers
$exe = Join-Path $InstallDir 'switchpro_vjoy.exe'
$vbs = Join-Path $InstallDir 'runhidden.vbs'
$vbsLine = 'CreateObject("Wscript.Shell").Run """' + $exe + '"" --auto", 0, False'
Set-Content -Path $vbs -Value $vbsLine -Encoding ASCII
Set-Content -Path (Join-Path $InstallDir 'SwitchProBridge.bat') -Value "@echo off`r`n`"$exe`" --auto`r`npause" -Encoding ASCII

# ---------- 5. HidHide (opt-in: hide the dead physical entry) ----------
$hidhideCli = 'C:\Program Files\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe'
if ($WithHidHide -and -not $NoHidHide) {
    if (-not (Test-Path $hidhideCli)) {
        $hh = Join-Path $PkgDir 'drivers\HidHide_1.5.230_x64.exe'
        if (Test-Path $hh) {
            Info "Installing HidHide..."
            # 7z SFX - try quiet flags, fall back to interactive
            $p = Start-Process $hh -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru -ErrorAction SilentlyContinue
            Start-Sleep 2
            if (-not (Test-Path $hidhideCli)) {
                Warn "HidHide silent install unavailable - please run drivers\HidHide_1.5.230_x64.exe manually, then re-run this script"
            }
        } else { Warn "HidHide installer missing from package - skipping" }
    }
    if (Test-Path $hidhideCli) {
        & $hidhideCli --cloak-on | Out-Null
        # whitelist feeder + hidhide itself
        & $hidhideCli --app-reg "`"$exe`"" | Out-Null
        & $hidhideCli --app-reg "`"$hidhideCli`"" | Out-Null
        # whitelist the game: explicit -GameExe, else every exe beside the
        # package folder's parent (the package normally sits inside the game dir)
        $games = @()
        if ($GameExe -and (Test-Path $GameExe)) { $games += $GameExe }
        else {
            $parent = Split-Path -Parent $PkgDir
            $games += Get-ChildItem -Path $parent -Filter '*.exe' -File -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName }
        }
        foreach ($g in ($games | Select-Object -Unique)) {
            & $hidhideCli --app-reg "`"$g`"" | Out-Null
            Info "Whitelisted for physical pad access: $g"
        }
        if (-not $GameExe -and $games.Count -eq 0) {
            Warn "No game exe found near the package - pass -GameExe `"C:\path\to\game.exe`" and re-run"
        }
        # hide every connected Nintendo Pro Controller (VID 057E PID 2009)
        $json = & $hidhideCli --dev-all | Out-String
        try {
            $devs = ($json | ConvertFrom-Json)
            $hidden = 0
            foreach ($group in $devs) {
                foreach ($d in $group.devices) {
                    if ($d.deviceInstancePath -match 'VID&0002057E_PID&2009') {
                        & $hidhideCli --dev-hide "`"$($d.deviceInstancePath)`"" | Out-Null
                        Info "Hid physical pad: $($d.deviceInstancePath)"
                        $hidden++
                    }
                }
            }
            if ($hidden -eq 0) {
                Warn "No Pro Controller currently connected - its dead entry won't be hidden."
                Warn "Pair the controller, wake it, then run HIDE-PADS.bat to hide it."
            }
        } catch { Warn "Could not parse HidHide device list: $($_.Exception.Message)" }
        Info "HidHide configured (physical pad hidden from apps except feeder/game)"
    }
} else {
    Warn "HidHide skipped (opt-in: pass -WithHidHide to enable)"
    Warn "joy.cpl will show the physical pad's dead entry next to the vJoy devices - harmless"
}

# ---------- 6. autostart ----------
if (-not $NoAutostart) {
    $runKey = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
    Set-ItemProperty -Path $runKey -Name 'SwitchProBridge' -Value "wscript.exe `"$vbs`""
    Info "Autostart enabled (hidden). Remove via registry Run key 'SwitchProBridge' or -NoAutostart"
}

# ---------- done ----------
Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "Pair your Pro Controller in Windows Bluetooth settings, then:"
Write-Host "  - it appears in joy.cpl as 'vJoy 1' (and 'vJoy 2' for a second pad)"
Write-Host "  - axes 1-4 = sticks, axis 3/6/7 = gyro XYZ, axis 8 = accel, hat = D-pad"
Write-Host "  - calibrate in joy.cpl like any joystick"
Write-Host ""
Write-Host "The feeder runs hidden at login. To watch it live, run:"
Write-Host "  $InstallDir\SwitchProBridge.bat"
Write-Host ""
if ($WithHidHide -and $GameExe) { Info "Game exe whitelisted for physical pad access: $GameExe" }
