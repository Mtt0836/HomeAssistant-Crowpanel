@echo off
rem Trascina uno o piu' video (o una cartella) su questo file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0converti_video.ps1" %*
pause
