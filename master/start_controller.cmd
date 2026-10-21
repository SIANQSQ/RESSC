@echo off
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
    python "%~dp0slider_controller.py" %*
) else (
    py -3 "%~dp0slider_controller.py" %*
)
if errorlevel 1 pause
