<#
  drag-probe.ps1 (v1.0) - Galaxy S10: what is slow while a window is dragged / resized (2026-11-12: "drag and resize
  lag"). Run ON THE PHONE in the desktop session, PowerShell as Administrator, from the kit folder:
      Set-ExecutionPolicy -Scope Process Bypass
      .\drag-probe.ps1
  Phase 1 (10 s): hands off - the idle baseline. Phase 2 (15 s, after the beep): drag a window around and resize it
  (corner) without stopping until the second beep. Each phase logs, side by side:
    - DWM: composed frames/s and missed frames (dwmrate.exe, DWM_TIMING_INFO)
    - MaliG76Wddm: presents (GPU / CPU copy), renders, submits, GPU busy share (kmdstats.exe)
    - clocks: LITTLE / MID / GPU / MIF granted by ACPM, temperatures (s10epowerctl), governor service state
    - CPU: per-core load, interrupt / DPC time, the 6 busiest processes, GPU engine utilisation
  Writes C:\S10Prov\drag-probe.txt - send that file back.
#>
$ErrorActionPreference = 'Continue'
$Out = 'C:\S10Prov\drag-probe.txt'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
"drag-probe.ps1 v1.0  $(Get-Date -Format s)" | Set-Content $Out -Encoding ascii
function W($m) { Write-Host $m; $m | Add-Content $Out -Encoding ascii }
function Tool($n) {
  foreach ($d in (Join-Path $PSScriptRoot 'tools\validate'), (Join-Path $PSScriptRoot 'tools'), 'C:\S10Prov\power', $PSScriptRoot) {
    $f = Join-Path $d $n; if (Test-Path $f) { return $f }
  }
  return $null
}
$dwmrate = Tool 'dwmrate.exe'; $kmd = Tool 'kmdstats.exe'; $ctl = Tool 's10epowerctl.exe'
W "tools: dwmrate $dwmrate | kmdstats $kmd | s10epowerctl $ctl"
foreach ($s in 'S10ePower', 'S10eGovernor') {
  $v = Get-CimInstance Win32_BaseService -Filter "Name='$s'" -ErrorAction SilentlyContinue
  W ("service {0}: {1}" -f $s, $(if ($v) { "$($v.State), start $($v.StartMode)" } else { 'not installed' }))
}
$gp = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\S10eGovernor\Parameters' -ErrorAction SilentlyContinue
if ($gp) { W ("governor: MaxLittle {0} MaxMid {1} MaxG3d {2} GpuFloor {3}" -f $gp.MaxLittle, $gp.MaxMid, $gp.MaxG3d, $gp.GpuFloor) }
$env0 = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' -ErrorAction SilentlyContinue
foreach ($n in 'MALIKM_TGSI_DIR', 'MALIUMD_NO_TC', 'MALIUMD_TC', 'GALLIUM_DRIVER') { if ($null -ne $env0.$n) { W "machine $n = '$($env0.$n)'" } }
$res = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object -First 1
if ($res) { W "display: $($res.CurrentHorizontalResolution)x$($res.CurrentVerticalResolution) $($res.CurrentRefreshRate) Hz, $($res.Name)" }

function Phase($name, $secs) {
  W ''
  W "===== $name ($secs s)"
  $jobs = @()
  $tmp = Join-Path $env:TEMP "dp-$($name -replace '\W', '')"
  if ($dwmrate) { $jobs += Start-Process $dwmrate -ArgumentList "$secs" -NoNewWindow -PassThru -RedirectStandardOutput "$tmp-dwm.txt" }
  if ($kmd) { $jobs += Start-Process $kmd -ArgumentList "busy $secs" -NoNewWindow -PassThru -RedirectStandardOutput "$tmp-busy.txt" }
  $prev = @{}; Get-Process | ForEach-Object { $prev[$_.Id] = $_.CPU }
  $t0 = Get-Date
  $samples = @()
  while (((Get-Date) - $t0).TotalSeconds -lt $secs) {
    try {
      $c = (Get-Counter -Counter '\Processor(*)\% Processor Time', '\Processor(_Total)\% Interrupt Time', '\Processor(_Total)\% DPC Time' -ErrorAction Stop).CounterSamples
      $samples += ,($c | ForEach-Object { '{0}={1:N0}' -f (($_.Path -replace '.*processor\(', '' -replace '\)\\% ', ' ') -replace ' time', ''), $_.CookedValue })
    } catch {}
    if ($ctl) { $samples += ,(& $ctl 2>&1 | Select-Object -First 2 | ForEach-Object { "$_" }) }
  }
  foreach ($j in $jobs) { [void]$j.WaitForExit(15000) }
  foreach ($s in $samples) { W ('  ' + ($s -join ' | ')) }
  Get-Content "$tmp-dwm.txt", "$tmp-busy.txt" -ErrorAction SilentlyContinue | ForEach-Object { W "  > $_" }
  if ($kmd) { & $kmd 2 2>&1 | Select-Object -First 3 | ForEach-Object { W "  kmd> $_" } }
  try {
    $g = (Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage' -ErrorAction Stop).CounterSamples |
         Sort-Object CookedValue -Descending | Select-Object -First 4
    W ('  gpu 3D: ' + (($g | ForEach-Object { '{0} {1:N0}%' -f ($_.InstanceName -replace '^pid_(\d+).*', 'pid $1'), $_.CookedValue }) -join ', '))
  } catch { W "  gpu 3D counter: $_" }
  $d = foreach ($p in Get-Process) { if ($null -ne $p.CPU) { [pscustomobject]@{ N = "$($p.Name)#$($p.Id)"; D = $p.CPU - [double]$prev[$p.Id] } } }
  W ('  top: ' + (($d | Sort-Object D -Descending | Select-Object -First 6 | ForEach-Object { '{0} {1:N1}s' -f $_.N, $_.D }) -join ', '))
}

W ''
W 'Phase 1: do NOT touch the screen for 10 s.'
Phase 'idle' 10
[console]::Beep(880, 300)
W 'Phase 2: NOW drag a window around and resize it from a corner, non-stop, until the second beep (15 s).'
Start-Sleep -Seconds 2
Phase 'drag-resize' 15
[console]::Beep(660, 300); [console]::Beep(660, 300)
W ''
W "done -> $Out"
