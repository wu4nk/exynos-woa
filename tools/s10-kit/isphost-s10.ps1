<#
  isphost-s10.ps1 (v1.1) - Galaxy S10, WINDOWS: Stage 4 option A on the phone itself (notes/s10-isp.md).
  Nothing here touches the camera hardware: no driver, no registers. Two user-mode tests:
      .\isphost-s10.ps1 x18      x18test.exe: does Windows leave x18 alone while foreign code holds a value in it?
                                 16 threads (more than the 8 cores: they get preempted and moved) keep a magic value
                                 in x18 for ~2 billion checks each. "X18 PRESERVED" = option A is possible at all.
                                 ~5-20 s, all cores busy.
      .\isphost-s10.ps1 host     isphost-test.exe: Samsung's ISP libraries run natively on Windows (relocated, x18
                                 swapped at every entry / exit) through the front-camera path - init, setfile,
                                 calibration, 30 frames - against FAKE register windows filled from the Android dump.
                                 Needs the phone's own libraries in C:\S10Prov\isp (copy them from s10cam.zip\files):
                                 fimc_is_lib.bin, fimc_is_rta.bin, setfile_3j1.bin, and optionally rom1_cal.bin.
      .\isphost-s10.ps1 all      both (x18 first; host only if x18 passed)
  Switches: -Lib <folder> (default C:\S10Prov\isp), -Frames N (default 30)
  Results: C:\S10Prov\isphost\ (x18test.txt, host.log, timing.txt, interface-calls.txt, regs-*.txt) and
  C:\S10Prov\isphost.zip - send that zip.
  Run from the folder this script came in (with x18test.exe, isphost-test.exe, sensor_itf_layout.txt, dump\).
#>
param(
    [Parameter(Position = 0)][ValidateSet('x18', 'host', 'all')][string]$Action = 'all',
    [string]$Lib = 'C:\S10Prov\isp',
    [int]$Frames = 30
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = 'C:\S10Prov\isphost'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Write-Host "isphost-s10 v1.1  ($Action)"
foreach ($f in 'x18test.exe', 'isphost-test.exe', 'sensor_itf_layout.txt', 'dump\3AA0.txt', 'dump\ISPLP.txt') {
    if (-not (Test-Path (Join-Path $here $f))) { throw "missing $f next to this script - unpack the whole zip" }
}

$x18ok = $true
if ($Action -eq 'x18' -or $Action -eq 'all') {
    Write-Host 'x18test: 16 threads x 2000 M checks ...'
    $p = Start-Process -FilePath (Join-Path $here 'x18test.exe') -ArgumentList '16', '2000' -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $out 'x18test.txt')
    Get-Content (Join-Path $out 'x18test.txt') | Select-Object -Last 4 | ForEach-Object { Write-Host "  $_" }
    $x18ok = ($p.ExitCode -eq 0)
    if (-not $x18ok) { Write-Host 'x18 NOT preserved: the native host cannot work this way - send the zip' -ForegroundColor Red }
}

$missing = @('fimc_is_lib.bin', 'fimc_is_rta.bin', 'setfile_3j1.bin') | Where-Object { -not (Test-Path (Join-Path $Lib $_)) }
if ($missing -and ($Action -ne 'x18')) {
    Write-Host "missing in ${Lib}: $($missing -join ', ') (copy them from s10cam.zip\files) - host test skipped" -ForegroundColor Yellow
}
if (-not $missing -and (($Action -eq 'host') -or ($Action -eq 'all' -and $x18ok))) {
    $hostOut = Join-Path $out 'run'
    if (Test-Path $hostOut) { Remove-Item -Recurse -Force $hostOut }
    $args2 = @($Lib, (Join-Path $here 'dump'), $hostOut, (Join-Path $here 'sensor_itf_layout.txt'))
    $cal = Join-Path $Lib 'rom1_cal.bin'
    if (Test-Path $cal) { $args2 += $cal; Write-Host "calibration: $cal" } else { Write-Host 'calibration: none (zero buffer)' }
    $env:FRAMES = "$Frames"
    $env:QUIET = '1'
    Write-Host "isphost-test: front path, $Frames frames ..."
    $q = $args2 | ForEach-Object { '"' + $_ + '"' }
    $p = Start-Process -FilePath (Join-Path $here 'isphost-test.exe') -ArgumentList $q -NoNewWindow -Wait -PassThru `
        -RedirectStandardError (Join-Path $out 'host-stderr.txt')
    Write-Host "exit code $($p.ExitCode)"
    $log = Join-Path $hostOut 'host.log'
    if (Test-Path $log) {
        Get-Content $log | Where-Object { $_ -match '^== |CRASH|steady|time-out|done:' } | Select-Object -Last 12 |
            ForEach-Object { Write-Host "  $_" }
    } else { Write-Host 'no host.log - the process died before writing it' -ForegroundColor Red }
}

$zip = 'C:\S10Prov\isphost.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$out\*" -DestinationPath $zip
Write-Host "result: $zip"
