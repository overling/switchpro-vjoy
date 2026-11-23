# hide-pads.ps1 - (re)apply HidHide rules for Nintendo controllers.
#
# Run this AFTER pairing a new Pro Controller / Joy-Con, or any time a
# physical pad shows up as a second dead entry in joy.cpl. It hides every
# connected Nintendo Bluetooth HID device from normal apps while keeping
# the bridge feeder (and optionally a game exe) able to see it.
#
# Usage:  hide-pads.ps1 [-GameExe PATH] [-InstallDir DIR]

param(
    [string]$GameExe   = "",
    [string]$InstallDir = "$env:LOCALAPPDATA\SwitchProBridge"
)

$ErrorActionPreference = 'Stop'

function Info($m) { Write-Host "[+] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[x] $m" -ForegroundColor Red; exit 1 }

# ---------- self-elevate ----------
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Requesting administrator rights (needed for HidHide)..."
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    foreach ($kv in $PSBoundParameters.GetEnumerator()) {
        $argList += "-$($kv.Key)"; if ($null -ne $kv.Value) { $argList += "`"$($kv.Value)`"" }
    }
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    exit
}

$hidhideCli = 'C:\Program Files\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe'
if (-not (Test-Path $hidhideCli)) {
    Fail "HidHide not installed. It is opt-in: re-run install.ps1 -WithHidHide, then re-run this."
}

$exe = Join-Path $InstallDir 'switchpro_vjoy.exe'
$PkgDir = Split-Path -Parent $MyInvocation.MyCommand.Path

& $hidhideCli --cloak-on | Out-Null
& $hidhideCli --app-reg "`"$exe`"" | Out-Null
& $hidhideCli --app-reg "`"$hidhideCli`"" | Out-Null

# Whitelist the game: explicit -GameExe, else every exe beside the package's
# parent folder (the package normally sits inside the game directory).
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

# Hide every connected Nintendo Bluetooth HID device (VID 057E).
# Pro Controller = PID 2009, Joy-Cons = 2006/2007.
$json = & $hidhideCli --dev-all | Out-String
try {
    $devs = ($json | ConvertFrom-Json)
    $hidden = 0
    foreach ($group in $devs) {
        foreach ($d in $group.devices) {
            if ($d.deviceInstancePath -match 'VID&0002057E') {
                & $hidhideCli --dev-hide "`"$($d.deviceInstancePath)`"" | Out-Null
                Info "Hid: $($d.deviceInstancePath)"
                $hidden++
            }
        }
    }
    if ($hidden -eq 0) {
        Warn "No Nintendo controllers found."
        Warn "Wake the controller (press any button) so Windows connects it, then re-run."
    } else {
        Info "Done - $hidden device(s) hidden. joy.cpl should now show only vJoy N entries."
    }
} catch {
    Fail "Could not parse HidHide device list: $($_.Exception.Message)"
}
