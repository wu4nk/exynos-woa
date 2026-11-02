@echo off
rem S10-install-once.cmd - placed in the all-users StartUp folder by push-to-windows (TWRP).
rem At logon: runs C:\S10Prov\kit\auto-install.ps1 elevated (Windows asks once: tap "Yes").
rem auto-install.ps1 writes install.done on success; then this file deletes itself.
rem After 3 failed tries it stops asking (log: C:\S10Prov\install.log).
set K=C:\S10Prov\kit
if exist "%K%\install.done" goto :clean
if not exist "%K%\auto-install.ps1" goto :clean
set /a T=0
if exist "%K%\install.tries" set /p T=<"%K%\install.tries"
if %T% GEQ 3 goto :eof
set /a T+=1
> "%K%\install.tries" echo %T%
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File C:\S10Prov\kit\auto-install.ps1'"
:clean
if exist "%K%\install.done" del "%~f0"
