@echo off
:: ──────────────────────────────────────────────────
::  Claude Token Meter — Companion setup (Windows)
:: ──────────────────────────────────────────────────
setlocal EnableDelayedExpansion

echo.
echo ╔══════════════════════════════════════════╗
echo ║  Claude Token Meter — Companion Setup   ║
echo ╚══════════════════════════════════════════╝
echo.

:: ── Check Python ──────────────────────────────────
where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found. Install from https://python.org
    pause
    exit /b 1
)

for /f "tokens=*" %%v in ('python --version 2^>^&1') do set PYVER=%%v
echo [OK] !PYVER! found

:: ── Create venv ───────────────────────────────────
set VENV=%~dp0.venv
if not exist "%VENV%" (
    echo [INFO] Creating virtual environment...
    python -m venv "%VENV%"
)

:: ── Install deps ──────────────────────────────────
echo [INFO] Installing dependencies...
"%VENV%\Scripts\pip" install --quiet --upgrade pip
"%VENV%\Scripts\pip" install --quiet -r "%~dp0requirements.txt"
echo [OK] Dependencies installed

:: ── Create launcher bat ───────────────────────────
set LAUNCHER=%~dp0run_monitor.bat
(
echo @echo off
echo set DIR=%%~dp0
echo "%%DIR%%.venv\Scripts\python" "%%DIR%%claude_monitor.py" %%*
) > "%LAUNCHER%"
echo [OK] Launcher created: companion\run_monitor.bat

echo.
echo ────────────────────────────────────────────
echo   Ready! Run the monitor:
echo.
echo   Auto-discover device:
echo     run_monitor.bat --discover
echo.
echo   Specify IP directly:
echo     run_monitor.bat --ip 192.168.1.42
echo.
echo   Test with simulated data:
echo     run_monitor.bat --ip claude-meter.local --simulate
echo ────────────────────────────────────────────
echo.
pause
