@echo off
rem SetupComplete.cmd (Galaxy S10 kit v1.2) - Windows Setup runs this as SYSTEM at the end of the
rem installation (C:\Windows\Setup\Scripts\). Installs the key agent the way the S10e's
rem s10e-setupcomplete.cmd does: S10eDiag (kernel service, ALIVE GPIO reads) + S10eAgent
rem (polls power / Bixby / volume every 25 ms, actions in C:\ProgramData\S10e\buttons.ini).
rem Log: C:\S10Prov\setupcomplete.log
rem v1.2 (make-windows v2.4-s10): the kit's finished GPU / power state, from the flags make-windows leaves in C:\S10Prov:
rem   final-gpu.flg    MaliG76Wddm full adapter (= gpu-s10.ps1 final): FullAdapter 1, GpuEnable 1, Wddm2 5, DirectFlip 1.
rem                    Setup / OOBE ran it display-only (the INF's NOCLOBBER defaults); the first logon restarts ONCE
rem                    (HKLM RunOnce) so the GPU comes up. (MS Learn "Add a custom script to Windows Setup": no restart
rem                    from SetupComplete.cmd itself.)
rem   final-power.flg  S10ePower + S10eGovernor at every boot (= power-s10.ps1 enable); final-oc.flg: GPU 806 MHz,
rem                    A55 2002 MHz - the clocks this S10 passed gputest / cpustress at (2026-11-12).
set P=C:\S10Prov
set LOG=%P%\setupcomplete.log
echo %date% %time% start > "%LOG%"

rem Display/sleep never, no lock screen, no hiberboot (as on the S10e: BasicDisplay cannot bring
rem the panel back once Windows turns it off).
for %%G in (3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e 8ec4b3a5-6868-48c2-be75-4f3044be88a7 29f6c1db-86da-48c5-9fdb-f2b67b1f44da 9d7815a6-7ee4-497e-8888-515a05f02364) do (
  reg add "HKLM\SOFTWARE\Policies\Microsoft\Power\PowerSettings\%%G" /v ACSettingIndex /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
  reg add "HKLM\SOFTWARE\Policies\Microsoft\Power\PowerSettings\%%G" /v DCSettingIndex /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
)
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\Personalization" /v NoLockScreen /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Power" /v HibernateEnabled /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power" /v HiberbootEnabled /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1

if not exist "%P%\S10eDiag\S10eDiag.sys" (echo no S10eDiag in %P% - keys not installed >> "%LOG%" & goto :done)
rem S10 kit v1.6: on an UPDATE the agent is running and Windows keeps its old .exe locked - copy /y then fails
rem silently and the old agent stays (2026-11-03: "Reboot to TWRP" still dead after the fixed agent was installed).
rem Stop it first; it is started again below.
sc stop S10eAgent >> "%LOG%" 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%P%\S10eDiag\S10eDiag.sys" C:\Windows\System32\drivers\ >> "%LOG%" 2>&1
for %%F in (s10ediag.exe S10eAgent.exe s10erotate.exe s10eperf.exe s10etouchlog.exe s10ereboot.exe s10escale.exe s10ekeys.exe) do copy /y "%P%\S10eDiag\%%F" C:\Windows\System32\ >> "%LOG%" 2>&1
sc create S10eDiag type= kernel start= system error= ignore binPath= System32\drivers\S10eDiag.sys DisplayName= "S10e diagnostics" >> "%LOG%" 2>&1
sc create S10eAgent type= own start= auto error= ignore binPath= "%%SystemRoot%%\System32\S10eAgent.exe" obj= LocalSystem DisplayName= "S10e diagnostics agent" >> "%LOG%" 2>&1
sc start S10eDiag >> "%LOG%" 2>&1
sc start S10eAgent >> "%LOG%" 2>&1
copy /y C:\Windows\System32\s10ereboot.exe "C:\Users\Public\Desktop\Reboot to TWRP.exe" >> "%LOG%" 2>&1

:done
if not exist "%P%\final-gpu.flg" goto :nogpu
set GP=HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters
reg add %GP% /v FullAdapter /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
reg add %GP% /v GpuEnable /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
reg add %GP% /v Wddm2 /t REG_DWORD /d 5 /f >> "%LOG%" 2>&1
reg add %GP% /v DirectFlip /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce /v S10GpuFinal /t REG_SZ /d "shutdown.exe /r /t 20 /c \"Galaxy S10: restarting once to switch the screen to the Mali GPU\"" /f >> "%LOG%" 2>&1
echo gpu final mode written, one restart at the first logon >> "%LOG%"
:nogpu
if not exist "%P%\final-power.flg" goto :nopower
if not exist "%P%\power\S10ePower.sys" (echo no %P%\power\S10ePower.sys >> "%LOG%" & goto :nopower)
set PK=HKLM\SYSTEM\CurrentControlSet\Services\S10ePower
set GK=HKLM\SYSTEM\CurrentControlSet\Services\S10eGovernor\Parameters
sc create S10ePower type= kernel start= auto binPath= "%P%\power\S10ePower.sys" DisplayName= "S10e Power (ACPM clocks)" >> "%LOG%" 2>&1
sc create S10eGovernor type= own start= auto binPath= "%P%\power\s10egovernor.exe" depend= S10ePower DisplayName= "S10e Governor" >> "%LOG%" 2>&1
reg add %PK% /v AllowSend /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
reg add %PK% /v AllowTmu /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
set MAXL=1950000
set MAXG=702000
if exist "%P%\final-oc.flg" (
  reg add %PK% /v GpuOc /t REG_DWORD /d 806000 /f >> "%LOG%" 2>&1
  reg add %PK% /v CpuOc /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
  set MAXL=2002000
  set MAXG=806000
)
rem = power-s10.ps1 v2.0 "enable" (+ the gpuoc / cpuoc ceilings)
rem (quoted pairs: "=" separates items in a FOR list)
for %%V in ("MaxLittle %MAXL%" "MaxMid 2314000" "MaxG3d %MAXG%" "BoostLittle 1742000" "BoostMid 2314000" "BoostG3d 650000" "EffLittle 1456000" "GpuFloor 260000") do (
  for /f "tokens=1,2" %%A in (%%V) do reg add %GK% /v %%A /t REG_DWORD /d %%B /f >> "%LOG%" 2>&1
)
copy /y "%P%\power\clocks.exe" "C:\Users\Public\Desktop\clocks.exe" >> "%LOG%" 2>&1
if not exist "%P%\final-gpu.flg" (sc start S10ePower >> "%LOG%" 2>&1 & sc start S10eGovernor >> "%LOG%" 2>&1)
echo power services installed (start at boot) >> "%LOG%"
:nopower
rem kit watcher (S10 kit v1.5): every logon offers a new ESP:\S10KIT (push-to-windows.cmd) for install
if exist "%P%\kit\kit-watch.ps1" (
  copy /y "%P%\kit\kit-watch.ps1" "%P%\kit-watch.ps1" >> "%LOG%" 2>&1
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$a=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\S10Prov\kit-watch.ps1'; $t=New-ScheduledTaskTrigger -AtLogOn; $p=New-ScheduledTaskPrincipal -GroupId 'BUILTIN\Users' -RunLevel Highest; Register-ScheduledTask -TaskName 'S10 kit watcher' -Action $a -Trigger $t -Principal $p -Force" >> "%LOG%" 2>&1
)
echo %date% %time% done >> "%LOG%"
