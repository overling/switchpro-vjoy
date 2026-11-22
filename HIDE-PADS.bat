@echo off
REM Hides newly-paired Nintendo controllers' dead entries in Windows.
REM Run this AFTER pairing an additional Pro Controller / Joy-Con.
REM Right-click -> "Run as administrator" if it doesn't elevate itself.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0hide-pads.ps1" %*
pause
