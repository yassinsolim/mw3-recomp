@echo off
rem One command: builds Modern Warfare 3 for Android and/or Windows from your
rem own disc image. Everything after setup.cmd is passed on (setup.cmd -Check,
rem setup.cmd -Iso D:\mw3.iso -Build both); README.md has the details.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
set code=%errorlevel%
if not "%code%"=="0" echo Setup stopped with an error; the logs are in out\logs.
pause
exit /b %code%
