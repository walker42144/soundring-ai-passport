@echo off
cd /d "%~dp0.."
python tools\soundring_pairing_gui.py
if errorlevel 1 (
  echo.
  echo Pairing tool could not start. Install Python and run:
  echo python -m pip install -r requirements-pairing.txt
  pause
)
