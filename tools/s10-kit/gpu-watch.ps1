<#
  gpu-watch.ps1 (v1.5) - runs by itself at every boot while gpu-s10.ps1 "gpu" / "gpu-dwm" mode is on (scheduled task
  "S10 GPU watch", SYSTEM, at startup). For boots where the phone is too slow to use (G2b-1 2026-11-03: no taskbar
  icons, no touch keyboard): nothing has to be typed, and nothing depends on the PRAM surviving (a TWRP boot destroys
  it). Every 15 s for 10 minutes it appends to C:\S10Prov\gpuwatch.txt (NTFS - readable from TWRP:
  adb pull /data/S10Prov/gpuwatch.txt):
    - MaliG76Wddm's PRAM heartbeat / stats lines (HB i<isr> d<dpc> v<vsync> s<submits> p<presents> ...) and its
      newest trace lines, read live with s10ediag.exe peek 0x8FE04000
    - % interrupt / DPC / processor time, interrupts/s
    - the 6 processes that used the most CPU in the last interval
  At +60 s it runs tools\m5test.exe once (one WRITE_VALUE job through WDDM, 60 s limit) and logs its output.
  v1.4 (upstream 1.0.72 black-panel hunt): every pass also logs MaliG76Wddm's own counters through its stats escape
  (tools\kmdstats.exe: presents, latch CPU copies into the framebuffer, GPU-presented, skipped, renders, IRQs), a raw
  line of the PRAM peek when it gives no trace, and 4 pixels of the framebuffer itself (s10ediag peek 0xCA000000 at
  the top / centre / bottom of the S10's 1440x3040 panel) - black panel = what the scan-out reads is black or not.
  v1.5 (2026-11-11, WDDM 2.0 black panel): DWM's own UMD log. Its presents stop within the first minute, so its
  malikm-dwm.exe-<pid>.txt stops growing and s10-log's "newest 60" (by time - the phone's clock restarts at the same
  time every boot) dropped it. Every pass now logs the running DWM's log size + last 12 lines, and copies the whole
  file to C:\S10Prov\dwmlog\ (s10-log v1.12 collects that folder).
  Each pass is written with Add-Content (file closed = flushed) so a forced reset loses at most the current pass.
#>
$ErrorActionPreference = 'Continue'
$Log = 'C:\S10Prov\gpuwatch.txt'
$Kit = Split-Path -Parent $MyInvocation.MyCommand.Path
$Diag = Get-Command s10ediag.exe -ErrorAction SilentlyContinue
function W($m) { $m | Add-Content $Log -Encoding ascii }

function Mali-Pram {
  if (-not $Diag) { return @('no s10ediag.exe') }
  $hex = & s10ediag.exe peek 0x8FE04000 16384 2>&1
  $bytes = New-Object System.Collections.Generic.List[byte]
  foreach ($line in $hex) {
    if ("$line" -notmatch '^[0-9a-fA-F]{10}:') { continue }
    foreach ($w in ("$line" -split ':')[1].Trim() -split '\s+') {
      if ($w -notmatch '^[0-9a-fA-F]{8}$') { continue }
      $v = [Convert]::ToUInt32($w, 16)
      0..3 | ForEach-Object { $bytes.Add([byte](($v -shr (8 * $_)) -band 0xFF)) }
    }
  }
  $txt = -join ($bytes | ForEach-Object { if ($_ -eq 10) { "`n" } elseif ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
  return @($txt -split "`n" | Where-Object { $_.Trim('. ') })
}

$up0 = [Environment]::TickCount
W ''
W "===== gpu-watch v1.5 boot $(Get-Date -Format s) (uptime $([int]($up0 / 1000)) s)"
try {
  $p = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters' -ErrorAction Stop
  W ("mode registry: FullAdapter {0} GpuEnable {1}  machine GALLIUM_DRIVER '{2}'" -f $p.FullAdapter, $p.GpuEnable,
     (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').GALLIUM_DRIVER)
} catch { W "mode registry: $_" }
$prev = @{}
Get-Process | ForEach-Object { $prev[$_.Id] = $_.CPU }
$m5done = $false
$cpdone = $false
$cpudone = $false
# v1.3: comprobe once more IN THE LOGGED-ON USER'S SESSION (run g2b3-12: from session 0 DComp gave E_ACCESSDENIED and
# the composition swap chain DXGI_ERROR_NOT_CURRENTLY_AVAILABLE - no desktop there). A one-shot task for the console
# user, interactive token, output to C:\S10Prov\comprobe-user.txt, removed afterwards.
function Invoke-ComprobeUser($exe) {
  $u = (Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue).UserName
  if (-not $u) { W '  comprobe(user): nobody logged on yet'; return $false }
  $out = 'C:\S10Prov\comprobe-user.txt'
  Remove-Item $out -Force -ErrorAction SilentlyContinue
  $name = 'S10 comprobe once'
  try {
    $act = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument ('/c ""' + $exe + '" > "' + $out + '" 2>&1"')
    $prn = New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive -RunLevel Limited
    $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 3)
    Register-ScheduledTask -TaskName $name -Action $act -Principal $prn -Settings $set -Force | Out-Null
    Start-ScheduledTask -TaskName $name
    for ($k = 0; $k -lt 60; $k++) {
      Start-Sleep -Seconds 2
      $st = (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue).State
      if ($k -ge 2 -and "$st" -ne 'Running') { break }
    }
    W "  comprobe(user $u): task state $st, last result $((Get-ScheduledTaskInfo -TaskName $name -ErrorAction SilentlyContinue).LastTaskResult)"
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
  } catch { W "  comprobe(user): $_"; return $true }
  Get-Content $out -ErrorAction SilentlyContinue | ForEach-Object { W "  uprobe> $_" }
  return $true
}

for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Seconds 15
  $up = [int]([Environment]::TickCount / 1000)
  $out = New-Object System.Collections.Generic.List[string]
  $out.Add("--- pass $i  uptime $up s  $(Get-Date -Format HH:mm:ss)")
  $pr = Mali-Pram
  $out.Add('  pram: ' + (($pr | Where-Object { $_ -match ' (HB|ST) ' }) -join ' | '))
  if ($Diag -and -not ($pr | Where-Object { $_ -match ' (HB|ST) ' })) {
    $raw = & s10ediag.exe peek 0x8FE04000 64 2>&1 | Select-Object -First 3
    $out.Add('  pram raw: ' + (($raw | ForEach-Object { "$_" }) -join ' / '))
  }
  # v1.4: the framebuffer the panel scans (GOP 0xCA000000, pitch 5760): row 100, row 1520, row 2940, centre column
  if ($Diag) {
    $px = foreach ($row in 100, 1520, 2940) {
      $a = 0xCA000000 + $row * 5760 + 720 * 4
      $h = & s10ediag.exe peek ('0x{0:X}' -f $a) 16 2>&1 | Where-Object { "$_" -match '^[0-9a-fA-F]{10}:' } | Select-Object -First 1
      "r$row " + $(if ($h) { ("$h" -split ':', 2)[1].Trim() } else { '?' })
    }
    $out.Add('  fb: ' + ($px -join ' | '))
    # DPU channel G0 (MW_DPU_G0_PA): enable, source/image size, scan-out base; DECON: global control, run status, trigger
    $g0 = & s10ediag.exe peek 0x19071000 0x50 2>&1 | Where-Object { "$_" -match '^[0-9a-fA-F]{10}:' }
    $dc = & s10ediag.exe peek 0x19030000 0x80 2>&1 | Where-Object { "$_" -match '^[0-9a-fA-F]{10}:' }
    $out.Add('  dpu g0: ' + (($g0 | ForEach-Object { "$_" }) -join ' / '))
    $out.Add('  decon: ' + (($dc | ForEach-Object { "$_" }) -join ' / '))
  }
  $ks = @((Join-Path $Kit 'tools\kmdstats.exe'), (Join-Path $Kit 'out-gpu\tools\kmdstats.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($ks) { & $ks 2 2>&1 | Select-Object -First 8 | ForEach-Object { $out.Add("  kmd> $_") } }
  $pr | Where-Object { $_ -notmatch ' (HB|ST) ' } | Select-Object -Last 6 | ForEach-Object { $out.Add("  pram> $_") }
  try {
    $c = (Get-Counter -Counter '\Processor(_Total)\% Processor Time', '\Processor(_Total)\% Interrupt Time',
          '\Processor(_Total)\% DPC Time', '\Processor(_Total)\Interrupts/sec' -ErrorAction Stop).CounterSamples
    $out.Add('  cpu: ' + (($c | ForEach-Object { '{0} {1:N1}' -f ($_.Path -replace '.*\\', ''), $_.CookedValue }) -join ' | '))
  } catch { $out.Add("  cpu: $_") }
  $now = @{}; $d = @()
  foreach ($proc in Get-Process) {
    $now[$proc.Id] = $proc.CPU
    if ($null -ne $proc.CPU) { $d += [pscustomobject]@{ N = $proc.Name; D = $proc.CPU - [double]($prev[$proc.Id]) } }
  }
  $prev = $now
  $out.Add('  top: ' + (($d | Sort-Object D -Descending | Select-Object -First 6 | ForEach-Object { '{0} {1:N1}s' -f $_.N, $_.D }) -join ', '))
  # v1.1: DWM's memory (run g2b3-10: one DWM died with c00001ad, out of memory) and the shell hosts' restarts
  $out.Add('  mem: ' + ((Get-Process dwm, explorer, SearchHost, StartMenuExperienceHost, TextInputHost -ErrorAction SilentlyContinue |
    ForEach-Object { '{0}#{1} priv {2:N0} MB ws {3:N0} MB up {4:N0} s' -f $_.Name, $_.Id, ($_.PrivateMemorySize64 / 1MB), ($_.WorkingSet64 / 1MB),
      ((Get-Date) - $_.StartTime).TotalSeconds }) -join ' | '))
  # v1.5: the running DWM's UMD log (gpu-s10 sets MALIKM_TGSI_DIR): size, last 12 lines, a copy for s10-log
  foreach ($dw in @(Get-Process dwm -ErrorAction SilentlyContinue)) {
    $f = "C:\ProgramData\S10e\umd\malikm-dwm.exe-$($dw.Id).txt"
    if (-not (Test-Path $f)) { $out.Add("  dwmlog: none for dwm#$($dw.Id) ($f)"); continue }
    try {
      $dst = 'C:\S10Prov\dwmlog'
      if (-not (Test-Path $dst)) { New-Item -ItemType Directory $dst | Out-Null }
      $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite, Delete')
      try { $ms = New-Object IO.MemoryStream; $fs.CopyTo($ms) } finally { $fs.Close() }
      [IO.File]::WriteAllBytes((Join-Path $dst (Split-Path $f -Leaf)), $ms.ToArray())
      $lines = [Text.Encoding]::ASCII.GetString($ms.ToArray()) -split "`r?`n" | Where-Object { $_ }
      $out.Add("  dwmlog: dwm#$($dw.Id) $($ms.Length) B, $($lines.Count) lines")
      $lines | Select-Object -Last 12 | ForEach-Object { $out.Add("  dwm> $_") }
      $err = "C:\ProgramData\S10e\umd\malikm-dwm.exe-$($dw.Id)-stderr.txt"
      if ((Test-Path $err) -and (Get-Item $err).Length -gt 0) {
        Copy-Item $err $dst -Force -ErrorAction SilentlyContinue
        Get-Content $err -Tail 6 -ErrorAction SilentlyContinue | ForEach-Object { $out.Add("  dwm stderr> $_") }
      }
    } catch { $out.Add("  dwmlog: $f - $_") }
  }
  ($out -join "`r`n") | Add-Content $Log -Encoding ascii

  if (-not $m5done -and $up -ge 60) {
    $m5done = $true
    $exe = @((Join-Path $Kit 'tools\m5test.exe'), (Join-Path $Kit 'out-gpu\tools\m5test.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($exe) {
      W "  m5test: $exe"
      $o = Join-Path $env:TEMP 'm5test-out.txt'
      $pp = Start-Process -FilePath $exe -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError "$o.err"
      if (-not $pp.WaitForExit(60000)) { $pp.Kill(); W '  m5test: TIMEOUT after 60 s (killed)' }
      else { W "  m5test exit $($pp.ExitCode)" }
      Get-Content $o, "$o.err" -ErrorAction SilentlyContinue | ForEach-Object { W "  m5> $_" }
    } else { W '  m5test: no tools\m5test.exe' }
  }

  # v1.2: comprobe (D3D11 / DXGI / D2D / DComp steps of the XAML hosts) once, after m5test - from here (SYSTEM,
  # session 0) because the logon task "S10 GPU fill" did not run in the GPU boots of runs g2b3-10/11. DComp may
  # refuse session 0; the D3D11 / D2D / shared-texture steps do not need a desktop.
  if ($m5done -and -not $cpdone -and $up -ge 90) {
    $cpdone = $true
    $exe = @((Join-Path $Kit 'tools\comprobe.exe'), (Join-Path $Kit 'out-gpu\tools\comprobe.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($exe) {
      $o = Join-Path $env:TEMP 'comprobe-out.txt'
      $pp = Start-Process -FilePath $exe -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError "$o.err"
      if (-not $pp.WaitForExit(90000)) { $pp.Kill(); W '  comprobe: TIMEOUT after 90 s (killed)' }
      else { W "  comprobe exit $($pp.ExitCode) (0 = all OK, 3 = a step failed)" }
      Get-Content $o, "$o.err" -ErrorAction SilentlyContinue | ForEach-Object { W "  probe> $_" }
    } else { W '  comprobe: no tools\comprobe.exe' }
  }
  if ($cpdone -and -not $cpudone -and $up -ge 120) {
    $exe = @((Join-Path $Kit 'tools\comprobe.exe'), (Join-Path $Kit 'out-gpu\tools\comprobe.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($exe) { $cpudone = Invoke-ComprobeUser $exe } else { $cpudone = $true }
  }
}
W "===== gpu-watch done (uptime $([int]([Environment]::TickCount / 1000)) s)"
