<#
  power-s10.ps1 (v1.4) - Galaxy S10: CPU / GPU clocks and temperatures through upstream's S10ePower (ACPM client) and
  S10eGovernor (load-following clocks, screen-off minimum, thermal guard at 83 C). Run ON THE PHONE, PowerShell as
  Administrator, from the kit folder that holds out\S10ePower\ (build-s10.cmd v1.13+).

    .\power-s10.ps1 test      1. installs S10ePower READ-ONLY (manual start) and checks the S10's ACPM channel table
                              (DVFS ch 0 and TMU ch 9 must be queue channels), 2. then with sends allowed: reads the
                              three temperatures, steps LITTLE / MID / GPU to their tops (1.95 / 2.314 GHz / 702 MHz)
                              for a 15 s cpustress, reads the temperatures again and puts the boot rates back.
                              Nothing starts by itself at the next boot.
    .\power-s10.ps1 enable    after a good "test": S10ePower + S10eGovernor start at every boot (the upstream S10e
                              settings: ceilings 1.95 / 2.314 GHz / 702 MHz, boost 1.742 / 2.106 GHz / 433 MHz)
    .\power-s10.ps1 disable   governor off, S10ePower read-only and manual - the next boot runs at the boot rates
    .\power-s10.ps1 status    services, temperatures and current clocks
    .\power-s10.ps1 gpuoc -Khz 806000   (or 754000; -Khz 0 = back to stock 702 MHz) - the YuROM Ultra GPU overclock:
                              S10ePower checks the ACPM G3D table layout exactly as that kernel does, points the
                              702 MHz level at PLL slot 0 (754 MHz, or rewritten to 806 MHz) WITHOUT changing its voltage,
                              then this script measures the real GPU clock (gpuclock.exe), runs gputest twice and keeps the
                              overclock only if nothing got worse; the governor's GPU ceiling follows. EXPERIMENTAL: 806 MHz
                              at 702 MHz's voltage depends on the chip ("silicon lottery"). Unload / disable = stock again.
    .\power-s10.ps1 cpuoc     (v1.4) the YuROM Ultra CPU "overclock" for the A55 cores: 2106 MHz is a level of the phone's
                              OWN ACPM table (frequency + Samsung's voltage for it) above the 1950 MHz the kernel normally
                              allows. Nothing is written to ACPM - S10ePower only allows that level after YuROM's checks
                              (it must be in the table, voltage <= 1175 mV and >= the 1950 MHz one). Then cpustress runs on
                              the A55s twice + all cores once; any error -> removed again. -Off = back to 1950 MHz.
                              (A75: 2400 MHz is already in use. M4 big cores: not running under Windows.)

  Every action writes C:\S10Prov\power-report.txt (send that file back). clocks.exe (live speeds) goes on the desktop.

  Why it is safe to try: the S10 is the same SoC as the S10e and its live device tree has the same values the driver
  hard-codes (checked 2026-11-11 against FdtBlob/beyond1lte.dtb): acpm_ipc reg 0x15900000 + 0x1000 / 0x2039000 +
  0x30000, initdata 0x7000; clock-controller acpm-ipc-channel 0 (DVFS); acpm_tmu channel 9; cluster0 cal-id
  0xB040002 1950000..442000 kHz, cluster1 0xB040003 2400000..507000; G3D cal-id 0xB04000A, gpu_dvfs_table 702000 ..
  156000 (the same 9 steps). The driver refuses any rate that is not on those ladders and caps them by temperature.
  Rescue if a boot goes wrong after "enable": TWRP is not needed - the services are plain services; boot, then
  .\power-s10.ps1 disable. If Windows does not come up at all: Safe Mode skips both (they are not boot-start).
#>
param([Parameter(Position = 0)][ValidateSet('test', 'enable', 'disable', 'status', 'gpuoc', 'cpuoc')][string]$Action = 'status',
      [int]$Khz = 806000, [switch]$Off)
$ErrorActionPreference = 'Stop'
$Report = 'C:\S10Prov\power-report.txt'
New-Item -ItemType Directory -Force (Split-Path $Report) | Out-Null
"power-s10.ps1 v1.4  $Action  $(Get-Date -Format s)" | Set-Content $Report -Encoding ascii
function Say($tag, $m, $c) { Write-Host "[$tag] $m" -ForegroundColor $c; "[$tag] $m" | Add-Content $Report }
function Ok($m)   { Say ' OK ' $m Green }
function Info($m) { Say 'INFO' $m Gray }
function Warn($m) { Say 'WARN' $m Yellow }
function Fail($m) { Say 'FAIL' $m Red; exit 1 }
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
      [Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'run PowerShell as Administrator' }

$Src = Join-Path $PSScriptRoot 'out\S10ePower'
$Dst = 'C:\S10Prov\power'
$PwrKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\S10ePower'
$GovKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\S10eGovernor'
$Ctl = Join-Path $Dst 's10epowerctl.exe'

function Run($exe, $argList) {
  $ErrorActionPreference = 'Continue'
  $o = & $exe @argList 2>&1 | ForEach-Object { "$_" }
  $ErrorActionPreference = 'Stop'
  $o | ForEach-Object { "    $_" | Add-Content $Report; Write-Host "    $_" }
  return $o
}
function Invoke-Sc($argList) { $ErrorActionPreference = 'Continue'; $o = & sc.exe @argList 2>&1 | Out-String; $ErrorActionPreference = 'Stop'; return $o }

function Copy-Files {
  if (-not (Test-Path (Join-Path $Src 'S10ePower.sys'))) { Fail "no $Src\S10ePower.sys - build the kit with build-s10.cmd v1.13+ and copy out\ next to this script" }
  New-Item -ItemType Directory -Force $Dst | Out-Null
  foreach ($s in 'S10eGovernor', 'S10ePower') { if (Get-Service $s -ErrorAction SilentlyContinue) { Invoke-Sc @('stop', $s) | Out-Null } }
  Start-Sleep -Seconds 1
  Copy-Item (Join-Path $Src '*') $Dst -Force
  $cer = Join-Path $PSScriptRoot 'out\kit-test-cert.cer'
  if (Test-Path $cer) { certutil -addstore -f Root $cer | Out-Null; certutil -addstore -f TrustedPublisher $cer | Out-Null }
  $lnk = Join-Path $env:PUBLIC 'Desktop\clocks.exe'
  Copy-Item (Join-Path $Dst 'clocks.exe') $lnk -Force -ErrorAction SilentlyContinue
  Ok "S10ePower files -> $Dst (clocks.exe on the desktop)"
}
function Ensure-Services([string]$Start) {
  if (-not (Get-Service S10ePower -ErrorAction SilentlyContinue)) {
    Invoke-Sc @('create', 'S10ePower', 'type=', 'kernel', "start=", $Start, 'binPath=', "$Dst\S10ePower.sys", 'DisplayName=', 'S10e Power (ACPM clocks)') | Out-Null
  } else { Invoke-Sc @('config', 'S10ePower', "start=", $Start, 'binPath=', "$Dst\S10ePower.sys") | Out-Null }
  if (-not (Get-Service S10eGovernor -ErrorAction SilentlyContinue)) {
    Invoke-Sc @('create', 'S10eGovernor', 'type=', 'own', "start=", $Start, 'binPath=', "$Dst\s10egovernor.exe", 'depend=', 'S10ePower', 'DisplayName=', 'S10e Governor') | Out-Null
  } else { Invoke-Sc @('config', 'S10eGovernor', "start=", $Start, 'binPath=', "$Dst\s10egovernor.exe", 'depend=', 'S10ePower') | Out-Null }
  if (-not (Get-Service S10ePower -ErrorAction SilentlyContinue)) { Fail 'sc create S10ePower failed' }
}
function Start-Power([int]$Send) {
  Set-ItemProperty $PwrKey AllowSend $Send -Type DWord
  Set-ItemProperty $PwrKey AllowTmu $Send -Type DWord
  if ((Get-Service S10ePower).Status -eq 'Running') { Invoke-Sc @('stop', 'S10ePower') | Out-Null; Start-Sleep -Seconds 1 }
  $o = Invoke-Sc @('start', 'S10ePower')
  Start-Sleep -Seconds 2
  if ((Get-Service S10ePower).Status -ne 'Running') { "    $o" | Add-Content $Report; Fail "S10ePower did not start: $($o.Trim())" }
}
function Show-Key {
  $k = Get-ItemProperty $PwrKey -ErrorAction SilentlyContinue
  foreach ($n in 'AcpmStatus', 'AllowSendActive', 'DvfsChannelOk', 'TmuChannelOk', 'AcpmApMax', 'AcpmChannelsOffset',
                 'AcpmPlugins', 'FvmapPlugin', 'FvmapOff', 'G3dOcCheck', 'G3dTopRate', 'G3dTopVolt', 'G3dPllLevels',
                 'G3dPms0', 'G3dPms1', 'GpuOc', 'GpuOcStatus', 'GpuOcActive', 'CpuOc', 'CpuOcStatus', 'CpuOcActive',
                 'LittleOcCheck', 'LittleOcUv', 'MidOcCheck', 'MidOcUv') {
    if ($null -ne $k.$n) { Info ("{0} = {1}" -f $n, $k.$n) }
  }
  foreach ($n in 'LittleLevels', 'MidLevels') {   # v1.4: the ACPM fvmap's CPU levels (rate kHz, voltage uV pairs)
    $b = $k.$n
    if ($b -is [byte[]]) {
      $l = for ($i = 0; $i + 7 -lt $b.Length; $i += 8) { '{0}/{1}' -f ([BitConverter]::ToUInt32($b, $i) / 1000), ([BitConverter]::ToUInt32($b, $i + 4) / 1000) }
      Info ("{0} (MHz/mV): {1}" -f $n, ($l -join ' '))
    }
  }
  return $k
}
function Gov([string]$What) {
  if (-not (Get-Service S10eGovernor -ErrorAction SilentlyContinue)) { return $false }
  $was = (Get-Service S10eGovernor).Status -eq 'Running'
  if ($What -eq 'stop' -and $was) { Invoke-Sc @('stop', 'S10eGovernor') | Out-Null; Start-Sleep -Seconds 2 }
  if ($What -eq 'start' -and -not $was) { Invoke-Sc @('start', 'S10eGovernor') | Out-Null; Start-Sleep -Seconds 2 }
  return $was
}
function GovParam([string]$Name, $Value) {
  if (-not (Test-Path "$GovKey\Parameters")) { New-Item -Force "$GovKey\Parameters" | Out-Null }
  if ($null -eq $Value) { Remove-ItemProperty "$GovKey\Parameters" $Name -ErrorAction SilentlyContinue }
  else { Set-ItemProperty "$GovKey\Parameters" $Name $Value -Type DWord }
}

switch ($Action) {
  'test' {
    Copy-Files
    Ensure-Services 'demand'
    Set-ItemProperty $GovKey Start 3 -Type DWord   # governor stays off during the test
    # stage 1: read-only (C0) - maps the ACPM SRAM and records the channel table, sends nothing
    Start-Power 0
    $k = Show-Key
    if ($k.AcpmStatus -and $k.AcpmStatus -ne 0) { Fail "ACPM SRAM not mapped (AcpmStatus $($k.AcpmStatus)) - stopping here, nothing was sent" }
    Ok 'stage 1 (read-only): ACPM SRAM mapped, channel table recorded'
    # stage 2: sends allowed (C1/C2) - the driver finds ch 0 / ch 9 in the live table itself and refuses otherwise
    Start-Power 1
    $k = Show-Key
    if ($k.DvfsChannelOk -ne 1) { Start-Power 0; Fail 'DVFS channel 0 is not a queue channel on this phone - back to read-only, send power-report.txt' }
    if ($k.TmuChannelOk -ne 1) { Warn 'TMU channel 9 not usable - no temperatures, so the driver keeps every clock at the boot rate (by design)' }
    Start-Sleep -Seconds 3
    Info 'state at the boot rates:'
    Run $Ctl @() | Out-Null
    foreach ($p in @(@('little', 1950000), @('mid', 2314000), @('g3d', 702000))) {
      Info "request $($p[0]) $($p[1]) kHz"
      Run $Ctl @('set', $p[0], "$($p[1])") | Out-Null
    }
    Run $Ctl @() | Out-Null
    $stress = Join-Path $Dst 'cpustress.exe'
    if (Test-Path $stress) { Info 'cpustress 15 s on all 6 cores (checks the cores stay correct at the top clocks):'; Run $stress @('15') | Out-Null }
    Info 'state after the load:'
    Run $Ctl @() | Out-Null
    foreach ($p in @(@('little', 1456000), @('mid', 1794000), @('g3d', 260000))) { Run $Ctl @('set', $p[0], "$($p[1])") | Out-Null }
    Ok 'boot rates restored. If the lines above show temperatures and cur = the requested rates (and cpustress PASS): .\power-s10.ps1 enable'
  }
  'enable' {
    Copy-Files
    Ensure-Services 'auto'
    Set-ItemProperty $PwrKey AllowSend 1 -Type DWord
    Set-ItemProperty $PwrKey AllowTmu 1 -Type DWord
    if (-not (Test-Path "$GovKey\Parameters")) { New-Item -Force "$GovKey\Parameters" | Out-Null }   # v-fix: -Force on an existing key wipes its values
    $gp = @{ MaxLittle = 1950000; MaxMid = 2314000; MaxG3d = 702000; BoostLittle = 1742000; BoostMid = 2314000; BoostG3d = 650000;   # v1.3: drag lag - touch = MID ceiling, GPU 650
             EffLittle = 1456000; GpuFloor = 260000 }
    # v1.2: keep the GPU overclock's ceiling (enable after gpuoc set MaxG3d back to 702000 - 806 was never used)
    $oc = (Get-ItemProperty $PwrKey -ErrorAction SilentlyContinue).GpuOc
    if ($oc -eq 754000 -or $oc -eq 806000) { $gp.MaxG3d = $oc; Info "GPU ceiling = the overclock ($oc kHz)" }
    if ((Get-ItemProperty $PwrKey -ErrorAction SilentlyContinue).CpuOc -eq 1) { $gp.MaxLittle = 2106000; Info 'A55 ceiling = 2106 MHz (cpuoc)' }   # v1.4
    foreach ($n in $gp.Keys) { Set-ItemProperty "$GovKey\Parameters" $n $gp[$n] -Type DWord }
    Start-Power 1
    $o = Invoke-Sc @('start', 'S10eGovernor')
    Start-Sleep -Seconds 3
    if ((Get-Service S10eGovernor).Status -ne 'Running') { Warn "S10eGovernor did not start: $($o.Trim())" } else { Ok 'S10eGovernor running' }
    Ok 'S10ePower + S10eGovernor start at every boot (upstream S10e ceilings). clocks.exe on the desktop shows the live speeds.'
  }
  'disable' {
    if (Get-Service S10eGovernor -ErrorAction SilentlyContinue) { Invoke-Sc @('stop', 'S10eGovernor') | Out-Null; Invoke-Sc @('config', 'S10eGovernor', 'start=', 'demand') | Out-Null }
    if (Get-Service S10ePower -ErrorAction SilentlyContinue) {
      Set-ItemProperty $PwrKey AllowSend 0 -Type DWord
      Invoke-Sc @('config', 'S10ePower', 'start=', 'demand') | Out-Null
    }
    Ok 'governor off, S10ePower read-only + manual - restart for the boot rates'
  }
  'gpuoc' {
    if ($Khz -ne 0 -and $Khz -ne 754000 -and $Khz -ne 806000) { Fail '-Khz must be 806000, 754000 or 0' }
    # v1.4: read BEFORE Copy-Files, which stops both services (v1.1-v1.3 never restarted the governor after gpuoc)
    $govWasOn = (Get-Service S10eGovernor -ErrorAction SilentlyContinue).Status -eq 'Running' -or (Get-Service S10eGovernor -ErrorAction SilentlyContinue).StartType -eq 'Automatic'
    Copy-Files
    if (-not (Get-Service S10ePower -ErrorAction SilentlyContinue)) { Ensure-Services 'demand' }
    if ($Khz -eq 0) { Remove-ItemProperty $PwrKey GpuOc -ErrorAction SilentlyContinue } else { Set-ItemProperty $PwrKey GpuOc $Khz -Type DWord }
    Start-Power 1
    $k = Show-Key
    if ($k.AcpmStatus -ne 0 -or $k.DvfsChannelOk -ne 1 -or $k.TmuChannelOk -ne 1) {
      Remove-ItemProperty $PwrKey GpuOc -ErrorAction SilentlyContinue; Start-Power 0
      Fail 'the ACPM channels are not all usable on this phone (see above) - back to read-only; run .\power-s10.ps1 test and send power-report.txt'
    }
    $vt = Join-Path $PSScriptRoot 'tools\validate'
    $gc = Join-Path $vt 'gpuclock.exe'; $gt = Join-Path $vt 'gputest.exe'
    if ($Khz -ne 0) {
      if ($k.GpuOcActive -ne $Khz) {
        Start-Power 1
        Fail ("overclock NOT applied (GpuOcStatus {0}, G3dOcCheck {1}): 0x10x = a table check failed (nothing written), 2 = sends off, 3 = no temperatures, 4 = read-back mismatch (restored). Send power-report.txt" -f $k.GpuOcStatus, $k.G3dOcCheck)
      }
      Ok ("ACPM G3D table patched: level 0 = {0} kHz at {1} uV (702 MHz's voltage), PLL slot 0 = 0x{2:x8}" -f $k.GpuOcActive, $k.G3dTopVolt, $(if ($Khz -eq 806000) { 0x007c0400 } else { 0x00740400 }))
    }
    $top = if ($Khz -eq 0) { 702000 } else { $Khz }
    Run $Ctl @('set', 'g3d', "$top") | Out-Null
    if (Test-Path $gc) { Info 'measured GPU clock (cycle counter):'; Run $gc @('1000', '3') | Out-Null } else { Warn "no $gc (gpu-validate kit) - clock not measured" }
    $bad = $false
    if ($Khz -ne 0 -and (Test-Path $gt)) {
      foreach ($r in 1, 2) {
        Info "gputest run $r at $top kHz:"
        $o = Run $gt @()
        $sum = $o | Where-Object { $_ -match '== (\d+) PASS' } | Select-Object -Last 1
        if (-not $sum -or [int]([regex]::Match($sum, '== (\d+) PASS').Groups[1].Value) -lt 91) { $bad = $true; Warn "gputest run ${r}: $sum (stock: 91 PASS)" }
      }
      Run $Ctl @() | Out-Null
      $tdr = Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; StartTime = (Get-Date).AddMinutes(-5) } -ErrorAction SilentlyContinue
      if ($tdr) { $bad = $true; Warn "Display (TDR) events in the last 5 minutes: $(@($tdr).Count)" }
    }
    if ($bad) {
      Remove-ItemProperty $PwrKey GpuOc -ErrorAction SilentlyContinue
      Start-Power 1
      $top = 702000
      Warn 'unstable at this clock -> overclock removed, stock 702 MHz table back'
    }
    if (-not (Test-Path "$GovKey\Parameters")) { New-Item -Force "$GovKey\Parameters" | Out-Null }
    Set-ItemProperty "$GovKey\Parameters" MaxG3d $top -Type DWord
    if ($govWasOn) { $o = Invoke-Sc @('start', 'S10eGovernor'); Start-Sleep -Seconds 2; Info "governor restarted (GPU ceiling $top kHz)" }
    if (-not $bad -and $Khz -ne 0) { Ok "GPU overclock $Khz kHz kept (survives reboots while S10ePower starts with GpuOc set; .\power-s10.ps1 gpuoc -Khz 0 removes it)" }
    elseif ($Khz -eq 0) { Ok 'stock GPU table (702 MHz top)' }
  }
  'cpuoc' {
    $govWasOn = (Get-Service S10eGovernor -ErrorAction SilentlyContinue).Status -eq 'Running' -or (Get-Service S10eGovernor -ErrorAction SilentlyContinue).StartType -eq 'Automatic'
    Copy-Files      # stops both services
    if (-not (Get-Service S10ePower -ErrorAction SilentlyContinue)) { Ensure-Services 'demand' }
    if ($Off) {
      Remove-ItemProperty $PwrKey CpuOc -ErrorAction SilentlyContinue
      Start-Power 1
      GovParam MaxLittle 1950000
      if ($govWasOn) { Gov 'start' | Out-Null }
      Ok 'A55 back to the stock 1950 MHz ceiling'
      break
    }
    Set-ItemProperty $PwrKey CpuOc 1 -Type DWord
    Start-Power 1
    $k = Show-Key
    if ($k.CpuOcActive -ne 2106000) {
      Remove-ItemProperty $PwrKey CpuOc -ErrorAction SilentlyContinue; Start-Power 1
      if ($govWasOn) { Gov 'start' | Out-Null }
      Fail ("2106 MHz NOT allowed (CpuOcStatus {0}, LittleOcCheck {1}): 6 = not a level of this phone's ACPM table, 9 = its voltage is outside YuROM's limits, 7 = the table does not match the A55 ladder, 2/3 = sends / temperatures off. Nothing changed. Send power-report.txt" -f $k.CpuOcStatus, $k.LittleOcCheck)
    }
    Ok ("A55 2106 MHz allowed: ACPM level with {0} mV (YuROM checks passed, nothing written to ACPM)" -f ($k.LittleOcUv / 1000))
    Run $Ctl @('set', 'little', '2106000') | Out-Null
    $cli = Join-Path $Dst 'clockscli.exe'
    if (Test-Path $cli) { Info 'measured clocks:'; Run $cli @('1', '1') | Out-Null }
    $stress = Join-Path $Dst 'cpustress.exe'
    $bad = $false
    foreach ($r in @(@('60', '0xf', 'A55 x4'), @('60', '0xf', 'A55 x4'), @('30', '0x3f', 'all 6 cores'))) {
      Info "cpustress $($r[0]) s on $($r[2]) at 2106 MHz:"
      $o = Run $stress @($r[0], $r[1])
      if (-not ($o -match '^PASS')) { $bad = $true; Warn "cpustress on $($r[2]): $(($o | Select-Object -Last 1))" }
      Run $Ctl @() | Out-Null
    }
    if ($bad) {
      Remove-ItemProperty $PwrKey CpuOc -ErrorAction SilentlyContinue
      Start-Power 1
      GovParam MaxLittle 1950000
      Warn 'computation errors at 2106 MHz -> removed, A55 back to 1950 MHz'
    } else {
      GovParam MaxLittle 2106000
      Ok 'A55 2106 MHz kept (governor ceiling 2106 MHz; survives reboots; .\power-s10.ps1 cpuoc -Off removes it)'
    }
    if ($govWasOn) { Gov 'start' | Out-Null; Info 'governor restarted' }
  }
  'status' {
    foreach ($s in 'S10ePower', 'S10eGovernor') {
      $v = Get-CimInstance Win32_BaseService -Filter "Name='$s'" -ErrorAction SilentlyContinue
      if ($v) { Info "$s : $($v.State), start $($v.StartMode)" } else { Info "$s : not installed" }
    }
    Show-Key | Out-Null
    if ((Get-Service S10ePower -ErrorAction SilentlyContinue).Status -eq 'Running') { Run $Ctl @() | Out-Null }
  }
}
Info "report: $Report"
