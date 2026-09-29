@echo off
rem Comprime la pagina dell'editor per la SD e per il firmware.
rem Puoi passare la lettera della SD, es.: costruisci_web.bat E:
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0costruisci_web.ps1" %*
pause
