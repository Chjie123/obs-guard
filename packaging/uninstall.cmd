@echo off
title obs-guard uninstaller
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
if errorlevel 1 pause
