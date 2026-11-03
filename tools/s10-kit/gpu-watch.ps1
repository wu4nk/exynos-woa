<#
  gpu-watch.ps1 (v1.0) - runs by itself at every boot while gpu-s10.ps1 "gpu" / "gpu-dwm" mode is on (scheduled task
  "S10 GPU watch", SYSTEM, at startup). For boots where the phone is too slow to use (G2b-1 2026-11-03: no taskbar
  icons, no touch keyboard): nothing has to be typed, and nothing depends on the PRAM surviving (a TWRP boot destroys
  it). Every 15 s for 10 minutes it appends to C:\S10Prov\gpuwatch.txt (NTFS - readable from TWRP:
  adb pull /data/S10Prov/gpuwatch.txt):
    - MaliG76Wddm's PRAM heartbeat / stats lines (HB i<isr> d<dpc> v<vsync> s<submits> p<presents> ...) and its
      newest trace lines, read live with s10ediag.exe peek 0x8FE04000
    - % interrupt / DPC / processor time, interrupts/s
    - the 6 processes that used the most CPU in the last interval
  At +60 s it runs tools\m5test.exe once (one WRITE_VALUE job through WDDM, 60 s limit) and logs its output.
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
W "===== gpu-watch v1.0 boot $(Get-Date -Format s) (uptime $([int]($up0 / 1000)) s)"
try {
  $p = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters' -ErrorAction Stop
  W ("mode registry: FullAdapter {0} GpuEnable {1}  machine GALLIUM_DRIVER '{2}'" -f $p.FullAdapter, $p.GpuEnable,
     (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').GALLIUM_DRIVER)
} catch { W "mode registry: $_" }
$prev = @{}
Get-Process | ForEach-Object { $prev[$_.Id] = $_.CPU }
$m5done = $false

for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Seconds 15
  $up = [int]([Environment]::TickCount / 1000)
  $out = New-Object System.Collections.Generic.List[string]
  $out.Add("--- pass $i  uptime $up s  $(Get-Date -Format HH:mm:ss)")
  $pr = Mali-Pram
  $out.Add('  pram: ' + (($pr | Where-Object { $_ -match ' (HB|ST) ' }) -join ' | '))
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
}
W "===== gpu-watch done (uptime $([int]([Environment]::TickCount / 1000)) s)"
