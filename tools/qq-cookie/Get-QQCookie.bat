@echo off
rem Deepstream - QQ Music Cookie Helper
rem Pure ASCII on purpose: cmd reads .bat with the system code page.
setlocal
title Deepstream - QQ Music Cookie Helper
cd /d "%~dp0"
chcp 65001 >nul 2>nul

where powershell >nul 2>nul
if errorlevel 1 (
  echo PowerShell not found. Windows 7+ should have it.
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Get-QQCookie.ps1" %*
set RC=%ERRORLEVEL%

echo.
if "%RC%"=="0" (
  echo Done. Press any key to close.
) else (
  echo Stopped with code %RC%.
  echo Press any key to close.
)
pause >nul
endlocal
exit /b %RC%
