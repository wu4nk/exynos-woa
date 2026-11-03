<#
  gpu-fill.ps1 (v1.0) - G2b-2 without typing: runs at logon in the user's session (scheduled task "S10 GPU fill",
  registered by gpu-s10.ps1 "gpu"; d3dfill needs the interactive session - session 0 owns no display). 45 s after
  logon it runs tools\d3dfill.exe 3 with GALLIUM_DRIVER=panfrost in ITS OWN environment: the Mesa UMD renders red,
  green, blue and white on the Mali (DWM stays on softpipe), and d3dfill reads pixel (0,0) of every phase back
  through a staging copy - proof that the GPU wrote it, whatever reaches the screen. Output appended to
  C:\S10Prov\d3dfill-auto.txt (TWRP: adb pull /data/S10Prov/d3dfill-auto.txt). 120 s limit.
#>
$ErrorActionPreference = 'Continue'
$Log = 'C:\S10Prov\d3dfill-auto.txt'
$Kit = Split-Path -Parent $MyInvocation.MyCommand.Path
function W($m) { $m | Add-Content $Log -Encoding ascii }

Start-Sleep -Seconds 45
W ''
W "===== gpu-fill v1.0 $(Get-Date -Format s) uptime $([int]([Environment]::TickCount / 1000)) s user $env:USERNAME"
$exe = @((Join-Path $Kit 'tools\d3dfill.exe'), (Join-Path $Kit 'out-gpu\tools\d3dfill.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { W 'no tools\d3dfill.exe'; exit 1 }
$o = Join-Path $env:TEMP 'd3dfill-out.txt'
$psi = New-Object Diagnostics.ProcessStartInfo $exe, '3'
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.EnvironmentVariables['GALLIUM_DRIVER'] = 'panfrost'
$p = [Diagnostics.Process]::Start($psi)
$so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(120000)) { $p.Kill(); W 'd3dfill: TIMEOUT after 120 s (killed)' } else { W "d3dfill exit $($p.ExitCode) (0 = every phase presented, 1 = no Mali adapter, 2 = a step failed)" }
foreach ($t in $so, $se) { if ($t.Wait(5000)) { $t.Result -split "`r?`n" | Where-Object { $_ } | ForEach-Object { W "  fill> $_" } } }
