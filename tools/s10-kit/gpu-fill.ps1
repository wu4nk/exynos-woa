<#
  gpu-fill.ps1 (v1.1) - G2b-2 without typing: runs at logon in the user's session (scheduled task "S10 GPU fill",
  registered by gpu-s10.ps1 "gpu"; d3dfill needs the interactive session - session 0 owns no display). 45 s after
  logon it runs tools\d3dfill.exe 3 with GALLIUM_DRIVER=panfrost in ITS OWN environment: the Mesa UMD renders red,
  green, blue and white on the Mali (DWM stays on softpipe), and d3dfill reads pixel (0,0) of every phase back
  through a staging copy - proof that the GPU wrote it, whatever reaches the screen. Output appended to
  C:\S10Prov\d3dfill-auto.txt (TWRP: adb pull /data/S10Prov/d3dfill-auto.txt). 120 s limit.
  v1.1: then tools\comprobe.exe (D3D11 / DXGI / D2D / DirectComposition steps of the XAML shell hosts, each HRESULT)
  into the same file - the taskbar, search and touch keyboard die with a stowed E_INVALIDARG (run g2b3-10).
#>
$ErrorActionPreference = 'Continue'
$Log = 'C:\S10Prov\d3dfill-auto.txt'
$Kit = Split-Path -Parent $MyInvocation.MyCommand.Path
function W($m) { $m | Add-Content $Log -Encoding ascii }

Start-Sleep -Seconds 45
W ''
W "===== gpu-fill v1.1 $(Get-Date -Format s) uptime $([int]([Environment]::TickCount / 1000)) s user $env:USERNAME"
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

# v1.1: comprobe, with the machine environment (Panfrost, as the shell hosts get it)
$cp = @((Join-Path $Kit 'tools\comprobe.exe'), (Join-Path $Kit 'out-gpu\tools\comprobe.exe'), (Join-Path $Kit 'out-gpu\comprobe.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cp) { W 'no tools\comprobe.exe'; exit 0 }
$psi = New-Object Diagnostics.ProcessStartInfo $cp
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [Diagnostics.Process]::Start($psi)
$so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(120000)) { $p.Kill(); W 'comprobe: TIMEOUT after 120 s (killed)' } else { W "comprobe exit $($p.ExitCode) (0 = every step OK, 1 = no hardware adapter, 2 = no device, 3 = a step failed)" }
foreach ($t in $so, $se) { if ($t.Wait(5000)) { $t.Result -split "`r?`n" | Where-Object { $_ } | ForEach-Object { W "  probe> $_" } } }
