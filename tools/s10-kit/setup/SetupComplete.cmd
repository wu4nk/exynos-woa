@echo off
rem SetupComplete.cmd (Galaxy S10 kit v1.1) - Windows Setup runs this as SYSTEM at the end of the
rem installation (C:\Windows\Setup\Scripts\). Installs the key agent the way the S10e's
rem s10e-setupcomplete.cmd does: S10eDiag (kernel service, ALIVE GPIO reads) + S10eAgent
rem (polls power / Bixby / volume every 25 ms, actions in C:\ProgramData\S10e\buttons.ini).
rem Log: C:\S10Prov\setupcomplete.log
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
copy /y "%P%\S10eDiag\S10eDiag.sys" C:\Windows\System32\drivers\ >> "%LOG%" 2>&1
for %%F in (s10ediag.exe S10eAgent.exe s10erotate.exe s10eperf.exe s10etouchlog.exe s10ereboot.exe s10escale.exe s10ekeys.exe) do copy /y "%P%\S10eDiag\%%F" C:\Windows\System32\ >> "%LOG%" 2>&1
sc create S10eDiag type= kernel start= system error= ignore binPath= System32\drivers\S10eDiag.sys DisplayName= "S10e diagnostics" >> "%LOG%" 2>&1
sc create S10eAgent type= own start= auto error= ignore binPath= "%%SystemRoot%%\System32\S10eAgent.exe" obj= LocalSystem DisplayName= "S10e diagnostics agent" >> "%LOG%" 2>&1
sc start S10eDiag >> "%LOG%" 2>&1
sc start S10eAgent >> "%LOG%" 2>&1
copy /y C:\Windows\System32\s10ereboot.exe "C:\Users\Public\Desktop\Reboot to TWRP.exe" >> "%LOG%" 2>&1

:done
rem kit watcher (S10 kit v1.5): every logon offers a new ESP:\S10KIT (push-to-windows.cmd) for install
if exist "%P%\kit\kit-watch.ps1" (
  copy /y "%P%\kit\kit-watch.ps1" "%P%\kit-watch.ps1" >> "%LOG%" 2>&1
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$a=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\S10Prov\kit-watch.ps1'; $t=New-ScheduledTaskTrigger -AtLogOn; $p=New-ScheduledTaskPrincipal -GroupId 'BUILTIN\Users' -RunLevel Highest; Register-ScheduledTask -TaskName 'S10 kit watcher' -Action $a -Trigger $t -Principal $p -Force" >> "%LOG%" 2>&1
)
echo %date% %time% done >> "%LOG%"
