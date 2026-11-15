@echo off
rem SetupComplete.cmd (Galaxy S10 kit v1.4) - Windows Setup runs this as SYSTEM at the end of the
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
rem v1.3 (2026-11-15, user: louder audio, better battery):
rem   audio            the speaker stages ON, as install-in-windows.ps1 does (the INF's defaults are 0 = silent, and
rem                    v1.2 never set them: a v2.8 image had no sound): AmpInit / AmpEnable / AboxEnable / Stereo 1,
rem                    no digital attenuation, analog gain +3 dB on both amps over the by-ear baseline (earpiece 12 -> 15,
rem                    loudspeaker 10 -> 13; 1 dB per step, Exynos9820Audio clamps at 17 = stock music level, which stock
rem                    runs behind Samsung's DSP speaker protection - we stay 2 steps under it). The speaker app
rem                    (S10eSpeakerTrim, compiled from the kit's .cs) goes on the desktop to set it by ear up to 17.
rem v1.4 (2026-11-16, user: no microphone in Sound settings): CodecEnable 1 - the driver builds the Microphone
rem                    (and 3.5 mm Headphones) endpoint only with the CS47L93 codec on (notes/d8-mic.md; S10 wiring = S10e).
rem   battery          background work Windows does on its own, cut (MS Learn Policy CSP / Set-MpPreference):
rem                    Store apps not in the background (Privacy/LetAppsRunInBackground 2 = force deny), Delivery
rem                    Optimization without peer-to-peer (DODownloadMode 0 = HTTP only), Defender scans at 20 % average
rem                    CPU, low priority, only when idle, no catch-up scans; battery saver from 30 %.
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
rem ---- v1.3: audio on, a little louder (see the header)
set AK=HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters
for %%V in ("AmpInit 1" "AmpEnable 1" "AboxEnable 1" "Stereo 1" "SpkAtten 0" "RcvAtten 0" "RcvGain 15" "SpkGain 13" "CodecEnable 1" "MicGain 24") do (
  for /f "tokens=1,2" %%A in (%%V) do reg add %AK% /v %%A /t REG_DWORD /d %%B /f >> "%LOG%" 2>&1
)
rem the driver reads Parameters when it starts: with the GPU restart pending that restart applies them, otherwise now
if not exist "%P%\final-gpu.flg" pnputil /restart-device "ACPI\SAMS9827\0" >> "%LOG%" 2>&1
set CSC=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe
if exist "%P%\kit\S10eSpeakerTrim.cs" if exist "%CSC%" "%CSC%" /nologo /target:winexe /platform:anycpu "/out:C:\Users\Public\Desktop\S10eSpeakerTrim.exe" "%P%\kit\S10eSpeakerTrim.cs" >> "%LOG%" 2>&1
echo audio on: stereo, gains 15 / 13 >> "%LOG%"

rem ---- v1.3: battery - less background work (see the header)
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\AppPrivacy" /v LetAppsRunInBackground /t REG_DWORD /d 2 /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\DeliveryOptimization" /v DODownloadMode /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-MpPreference -ScanAvgCPULoadFactor 20 -EnableLowCpuPriority $true -ScanOnlyIfIdleEnabled $true -DisableCatchupFullScan $true -DisableCatchupQuickScan $true" >> "%LOG%" 2>&1
powercfg /setdcvalueindex SCHEME_CURRENT SUB_ENERGYSAVER ESBATTTHRESHOLD 30 >> "%LOG%" 2>&1
powercfg /setactive SCHEME_CURRENT >> "%LOG%" 2>&1
echo battery settings written >> "%LOG%"
rem kit watcher (S10 kit v1.5): every logon offers a new ESP:\S10KIT (push-to-windows.cmd) for install
if exist "%P%\kit\kit-watch.ps1" (
  copy /y "%P%\kit\kit-watch.ps1" "%P%\kit-watch.ps1" >> "%LOG%" 2>&1
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$a=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\S10Prov\kit-watch.ps1'; $t=New-ScheduledTaskTrigger -AtLogOn; $p=New-ScheduledTaskPrincipal -GroupId 'BUILTIN\Users' -RunLevel Highest; Register-ScheduledTask -TaskName 'S10 kit watcher' -Action $a -Trigger $t -Principal $p -Force" >> "%LOG%" 2>&1
)
echo %date% %time% done >> "%LOG%"
