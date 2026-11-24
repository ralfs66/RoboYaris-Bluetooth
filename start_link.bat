@echo off
cd /d "%~dp0"
set "PY=C:\Program Files\Python314\python.exe"
if not exist "%PY%" set "PY=python"
"%PY%" -u "%~dp0start_link.py"
echo.
pause
