@echo off
rem Trascina una o piu' foto (o una cartella) su questo file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0converti_foto.ps1" %*
pause
