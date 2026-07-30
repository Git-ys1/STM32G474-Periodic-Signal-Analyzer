@echo off
setlocal
cd /d "%~dp0.."
python tools\custom_waveform_lab.py
if errorlevel 1 pause
endlocal
