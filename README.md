# switchpro-vjoy

Feeds Nintendo Switch Pro Controllers (and Joy-Cons) into
[vJoy](https://github.com/njz3/vJoy) virtual joysticks so Windows sees
them as normal, calibratable devices — sticks, buttons, D-pad, **gyro**
and **accelerometer** included.

No custom kernel driver, no Secure Boot changes, no cost. The only
drivers installed are the vendors' own signed releases (vJoy, HidHide).

## Why

A Pro Controller over Bluetooth enumerates fine in Windows but its
HID report format is Nintendo-proprietary: `joy.cpl` and DirectInput
apps see a device that never moves, and nothing exposes its motion
sensors. SDL3 parses it natively (which is why games work), so this
tool reads the pad through SDL3 and re-emits the state through vJoy.

## Usage

```
switchpro_vjoy.exe              auto mode: every Nintendo pad gets its
                                own vJoy device (vJoy 1, vJoy 2, ...)
switchpro_vjoy.exe --list       list detected pads + unique device paths
switchpro_vjoy.exe --auto       same as no args
switchpro_vjoy.exe <match> [vjd]  single mode: first pad matching a
                                name/path substring -> vJoy device <vjd>
```

Auto mode is the normal way to run it: it waits for controllers,
assigns each a free vJoy device, and re-adopts pads after reconnect.

## Axis map (per vJoy device)

| Axis | Source |
|---|---|
| 1 X, 2 Y | left stick |
| 3 Z | gyro pitch |
| 4 Rx, 5 Ry | right stick |
| 6 Rz | gyro yaw |
| 7 Slider0 | gyro roll |
| 8 Slider1 | accelerometer Y (gravity included) |

Buttons 1–16: face buttons, shoulders, stick clicks, capture/home.
Triggers are thresholded onto buttons 7/8. POV hat = D-pad (4-way).

## Requirements

- Windows 10/11 x64, Secure Boot can stay on
- [vJoy](https://github.com/njz3/vJoy) driver installed (signed)
- SDL3 is statically linked; `vJoyInterface.dll` is embedded in the exe
  and extracted to `%TEMP%` on first run if not found beside it

## Build

MSYS2 UCRT64:

```
windres -i vjoyres.rc -o vjoyres.res -O coff
gcc -O2 -o switchpro_vjoy.exe switchpro_vjoy.c vjoyres.res \
    -I/c/msys64/ucrt64/include /c/msys64/ucrt64/lib/libSDL3.a \
    -lwinmm -limm32 -lversion -luuid -ladvapi32 -lsetupapi -lole32 \
    -loleaut32 -luser32 -lgdi32 -lshell32 -lwinspool -lcomdlg32 \
    -ldinput8 -ldxguid -lksuser -liconv -lws2_32
```

`vJoyInterface.dll` (from the vJoy SDK or `C:\Program Files\vJoy\x64\`
after install) must sit next to `vjoyres.rc` at build time — it is
embedded into the exe, not committed.

## Installer

`install.ps1` automates the whole setup: silent vJoy + HidHide driver
install, vJoy device configuration, `vJoy N` device naming, feeder
deploy to `%LOCALAPPDATA%\SwitchProBridge`, autostart registration,
and hiding the physical pad's dead entry via HidHide.

```
install.ps1 [-InstallDir DIR] [-GameExe PATH] [-Devices N]
            [-NoHidHide] [-NoAutostart]
```

Place `vJoySetup.exe` and `HidHide_*.exe` (from their official
releases) in a `drivers/` folder next to the script.

`hide-pads.ps1` (with `HIDE-PADS.bat` wrapper) re-applies HidHide
rules for controllers paired *after* install — run it once whenever
a new pad shows up as a dead entry in `joy.cpl`.

## Notes

- vJoy reports are change-driven — the feeder streams continuously;
  one-shot writes can be dropped.
- Pad→vJoy numbering follows connection order and can reshuffle on
  reconnect; calibrate by the `vJoy N` name.
- Gyro arrives as axes 3/6/7 — apps wanting real HID sensor usages
  should read the physical pad via SDL/hidapi instead.
- SDL gamepad enumeration does not see vJoy devices, so games using
  SDL are unaffected whether the bridge runs or not.
