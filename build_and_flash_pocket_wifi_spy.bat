@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\build_and_flash_pocket_wifi_spy.ps1" %*
