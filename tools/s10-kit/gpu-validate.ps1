<#
  gpu-validate.ps1 (v1.3) - Galaxy S10: one run of every GPU check that needs no typing, after the upstream stack
  (KMD 1.0.72.1 as WDDM 2.0 + UMD 0222-s10.x) is up with DWM on the Mali ("gpu-s10.ps1 gpu-dwm" + reboot).
  Run ON THE PHONE from the kit folder, PowerShell as Administrator, IN THE DESKTOP SESSION (gltest needs a window):
      Set-ExecutionPolicy -Scope Process Bypass
      .\gpu-validate.ps1
  Writes C:\S10Prov\gpu-validate.txt (send that file back, or run s10-log afterwards - v1.12 does not pick it up,
  so send it directly). Takes about 3 minutes; the screen may flicker during the full-screen tests.

  What it checks:
    1. setup: GPU0 status, installed MaliG76Wddm version, Parameters (FullAdapter/GpuEnable/Wddm2/DirectFlip), the
       machine variables that must NOT be set in normal use (GALLIUM_DRIVER, MALIUMD_NO_TC, MALIKM_TGSI_DIR, ...),
       System32 / SysWOW64 UMD hashes
    2. dxdiag: driver model (WDDM 2.0 expected), feature levels (11_0 at the top expected), DDI version
    3. D3D11 tests (tools\validate\*.exe, ARM64): every one compares against the expected picture/values itself and
       exits 0 on PASS. d3dsm41 is 7/8 on the S10e as well (Bifrost LOD), so its exit 1 is a known result.
    4. the same kind of tests for 32-bit x86 apps (tools\validate\x86\*32.exe on SysWOW64\maliumd32.dll)
    5. OpenGL (gltest.exe on maligl.dll): GL strings + "GLTEST n/m PASS"
    6. timing: vblank (refresh interval), dwmrate (DWM compositions/s for 5 s), gpuclock (the GPU's clock),
       kmdstats (KMD counters)
    7. crashes since this boot: WER reports / Application Error events of dwm, LogonUI, explorer and the tests
  v1.1 (2026-11-12, first S10 run): "known" counter renamed (PowerShell names are case-insensitive: $known was the
  $Known table -> "System.Collections.Hashtable known"); d3dbc runs "lossy" (BC4/BC5 are re-compressed to ETC2/EAC
  since G26 - its plain mode differs by design, upstream g27 notes); gputest's whole output and every FAIL line are
  kept; x86 tools rebuilt (0xC0000139 = the tools' own d3dcompiler import, not the driver); every test runs with
  MALIKM_TGSI_DIR = C:\S10Prov\gpu-validate\logs (the UMD's per-process logs) and a failed test's log is listed;
  gltest uses tools\validate\maligl-diag.dll (logs why D3DKMTLock fails) in place of System32\maligl.dll for its
  run and puts the original back; everything goes into C:\S10Prov\gpu-validate.zip.
  v1.2: -TryUmd <folder> tests a NEW user-mode driver without installing it: the folder's maliumd.dll /
  maliumd32.dll / maligl.dll replace the System32 / SysWOW64 copies for the test run only (the in-use originals are
  renamed to *.s10prev - DWM keeps the copy it loaded at boot, so the desktop is not touched) and are put back
  afterwards; -Keep leaves the new files in place (then rebuild the kit package with them: gpu-s10 "gpu" would
  otherwise restore the package's copies).
  v1.3: FIX - v1.1's maligl-diag swap kept a $swapped flag, and PowerShell names are case-insensitive: it overwrote
  v1.2's $Swapped list, so -TryUmd never put the installed UMD back (2026-11-12 01:32: "RESTORE FAILED for True").
  The diag swap is gone (UMD 0222-s10.2's maligl logs the Lock / Lock2 results itself); the list is $TrySwapped.
  -Restore puts every *.s10prev back (System32 maliumd / maligl, SysWOW64 maliumd32) and exits.
  d3dsm5: when it fails, "d3dsm5 bits" runs 3 more times (2026-11-12: bits all zero once, everything else ok).
  Not automatic (look yourself, write the result to the chat): Edge -> edge://gpu ("Hardware accelerated" lines),
  a WebGL page, a YouTube video, scrolling in Settings.
#>
param([int]$Timeout = 60, [string]$TryUmd = '', [switch]$Keep, [switch]$Restore)
$ErrorActionPreference = 'Continue'
$Report = 'C:\S10Prov\gpu-validate.txt'
New-Item -ItemType Directory -Force (Split-Path $Report) | Out-Null
"gpu-validate.ps1 v1.3  $(Get-Date -Format s)  uptime $([int]([Environment]::TickCount / 1000)) s" | Set-Content $Report -Encoding ascii
function W($m) { Write-Host $m; $m | Add-Content $Report -Encoding ascii }
$Vd = Join-Path $PSScriptRoot 'tools\validate'
$pass = 0; $fail = 0; $nKnown = 0; $miss = 0
$Dir = 'C:\S10Prov\gpu-validate'; Remove-Item $Dir -Recurse -Force -ErrorAction SilentlyContinue
$Logs = Join-Path $Dir 'logs'; New-Item -ItemType Directory -Force $Logs | Out-Null
$env:MALIKM_TGSI_DIR = $Logs   # child processes inherit it: every test writes its UMD log there
$KnownFail = @{ 'd3dsm41.exe' = 'S10e: 7/8 (Bifrost LOD)' }

function Run-Test($exe, [string[]]$a = @(), [string]$label = '') {
  $name = Split-Path $exe -Leaf
  if (-not $label) { $label = $name }
  if (-not (Test-Path $exe)) { W ("  MISS {0,-22} (not in the kit)" -f $label); $script:miss++; return }
  $o = Join-Path $env:TEMP "gv-$name.out"
  $t0 = Get-Date
  $argStr = if ($a.Count) { $a -join ' ' } else { $null }
  $p = if ($argStr) { Start-Process -FilePath $exe -ArgumentList $argStr -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError "$o.err" -WorkingDirectory (Split-Path $exe) }
       else { Start-Process -FilePath $exe -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError "$o.err" -WorkingDirectory (Split-Path $exe) }
  $null = $p.Handle   # keeps the process handle so ExitCode is filled in (Start-Process -PassThru quirk)
  $ok = $p.WaitForExit($Timeout * 1000)
  $ms = [int]((Get-Date) - $t0).TotalMilliseconds
  $out = @(Get-Content $o, "$o.err" -ErrorAction SilentlyContinue)
  if (-not $ok) { try { $p.Kill() } catch {} ; $res = 'HANG'; $script:fail++ }
  elseif ($p.ExitCode -eq 0) { $res = 'PASS'; $script:pass++ }
  elseif ($KnownFail.ContainsKey($name)) { $res = 'KNOWN'; $script:nKnown++ }
  else { $res = 'FAIL'; $script:fail++ }
  $code = if ($ok) { $p.ExitCode } else { 'timeout' }
  W ("  {0,-5} {1,-22} exit {2}  {3} ms{4}" -f $res, $label, $code, $ms, $(if ($res -eq 'KNOWN') { "  ($($KnownFail[$name]))" } else { '' }))
  $show = if ($res -eq 'PASS') { 3 } else { 25 }
  $out | Set-Content (Join-Path $Dir "$label.txt") -Encoding ascii
  $fl = @($out | Where-Object { "$_" -match '^\s*(FAIL|DIFF)\b' })
  if ($res -ne 'PASS' -and $fl.Count) { $fl | Select-Object -First 30 | ForEach-Object { W "        ! $_" } }
  $out | Where-Object { "$_".Trim() } | Select-Object -Last $show | ForEach-Object { W "        | $_" }
  if ($res -ne 'PASS') {
    $base = [IO.Path]::GetFileNameWithoutExtension($name)
    Get-ChildItem $Logs -Filter "malikm-$base.exe-*" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $t0.AddSeconds(-2) } |
      ForEach-Object { W "        log: $($_.Name) ($($_.Length) B)"; Get-Content $_.FullName -Tail 8 | ForEach-Object { W "        log> $_" } }
  }
}

# v1.2: swap in a new UMD for this run (new processes load it; DWM keeps its boot-time copy)
$TrySwapped = @()
function Swap-In($src, $dst) {
  if (-not (Test-Path $src) -or -not (Test-Path $dst)) { return }
  takeown /f $dst /a | Out-Null; icacls $dst /grant '*S-1-5-32-544:F' | Out-Null
  $prev = "$dst.s10prev"
  if (Test-Path $prev) { W "  TryUmd: $prev exists (an earlier run did not put it back) - run .\gpu-validate.ps1 -Restore first, $dst not swapped"; return }
  try {
    Rename-Item $dst (Split-Path $prev -Leaf) -ErrorAction Stop
    Copy-Item $src $dst -Force -ErrorAction Stop
    $script:TrySwapped += $dst
    W ("  TryUmd: {0} <- {1} (sha256 {2})" -f $dst, $src, (Get-FileHash $dst).Hash.ToLower().Substring(0, 16))
  } catch {
    W "  TryUmd: $dst NOT swapped: $_"
    if (-not (Test-Path $dst) -and (Test-Path $prev)) { Rename-Item $prev (Split-Path $dst -Leaf) -ErrorAction SilentlyContinue }
  }
}
function Swap-Back {
  foreach ($dst in $script:TrySwapped) {
    # renamed away, not deleted: a process started during the run may still have the new DLL loaded
    try {
      $old = "$dst.s10new"; if (Test-Path $old) { Remove-Item $old -Force -ErrorAction SilentlyContinue }
      if (Test-Path $old) { $old = "$dst.s10new$(Get-Random)" }
      Rename-Item $dst (Split-Path $old -Leaf) -ErrorAction Stop
      Rename-Item "$dst.s10prev" (Split-Path $dst -Leaf) -ErrorAction Stop; W "  TryUmd: $dst restored"
    }
    catch { W "  TryUmd: RESTORE FAILED for $dst - rename $dst.s10prev back by hand: $_" }
  }
}
if ($Restore) {
  W '== Restore: *.s10prev back in place'
  foreach ($dst in 'C:\Windows\System32\maliumd.dll', 'C:\Windows\SysWOW64\maliumd32.dll', 'C:\Windows\System32\maligl.dll') {
    if (-not (Test-Path "$dst.s10prev")) { W "  $dst : no .s10prev, left as is"; continue }
    foreach ($f in $dst, "$dst.s10prev") { if (Test-Path $f) { takeown /f $f /a | Out-Null; icacls $f /grant '*S-1-5-32-544:F' | Out-Null } }
    $script:TrySwapped = @($dst); Swap-Back
  }
  exit 0
}
if ($TryUmd) {
  $tu = if ([IO.Path]::IsPathRooted($TryUmd)) { $TryUmd } else { Join-Path $PSScriptRoot $TryUmd }
  W "== 0. TryUmd $tu"
  Swap-In (Join-Path $tu 'maliumd.dll') 'C:\Windows\System32\maliumd.dll'
  Swap-In (Join-Path $tu 'maliumd32.dll') 'C:\Windows\SysWOW64\maliumd32.dll'
  Swap-In (Join-Path $tu 'maligl.dll') 'C:\Windows\System32\maligl.dll'
}

W '== 1. setup'
$g = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ACPI\SAMS9828\*' } | Select-Object -First 1
if ($g) { W "  GPU0: $($g.FriendlyName)  status $($g.Status)  problem $($g.Problem)" } else { W '  GPU0: NOT PRESENT (nogpu.flg on the ESP?)' }
$drv = Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.DeviceID -like 'ACPI\SAMS9828\*' } | Select-Object -First 1
if ($drv) { W "  driver: $($drv.DriverVersion) ($($drv.InfName))" }
$pp = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters' -ErrorAction SilentlyContinue
W ("  Parameters: FullAdapter {0} GpuEnable {1} Wddm2 {2} DirectFlip {3}" -f $pp.FullAdapter, $pp.GpuEnable, $pp.Wddm2, $pp.DirectFlip)
$env0 = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' -ErrorAction SilentlyContinue
foreach ($n in 'GALLIUM_DRIVER', 'MALIUMD_NO_TC', 'MALIUMD_TC', 'MALIKM_TGSI_DIR', 'S10_FAULTDUMP', 'S10_THUMBS') {
  if ($null -ne $env0.$n) { W "  machine $n = '$($env0.$n)'  <- debug/diagnosis setting" }
}
foreach ($f in 'C:\Windows\System32\maliumd.dll', 'C:\Windows\SysWOW64\maliumd32.dll', 'C:\Windows\System32\maligl.dll') {
  if (Test-Path $f) { W ("  {0}  {1:N0} B  sha256 {2}" -f $f, (Get-Item $f).Length, (Get-FileHash $f).Hash.ToLower().Substring(0, 16)) } else { W "  $f  missing" }
}

W '== 2. dxdiag (about 20 s)'
$dx = Join-Path $env:TEMP 'gv-dxdiag.txt'
Remove-Item $dx -Force -ErrorAction SilentlyContinue
$p = Start-Process dxdiag.exe -ArgumentList "/t `"$dx`"" -PassThru
[void]$p.WaitForExit(90000)
for ($k = 0; $k -lt 30 -and -not (Test-Path $dx); $k++) { Start-Sleep -Seconds 1 }
if (Test-Path $dx) {
  Get-Content $dx | Where-Object { $_ -match '^\s*(Card name|Driver Model|Feature Levels|DDI Version|Driver Version|Hardware Scheduling|Current Mode|Driver File Version)\s*:' } |
    Select-Object -First 12 | ForEach-Object { W "  $($_.Trim())" }
} else { W '  dxdiag wrote nothing' }

W '== 3. D3D11 tests (ARM64)'
foreach ($t in 'd3dcaps', 'd3dmsaa', 'd3dcs', 'd3dsm41', 'd3dsm5', 'd3dgs', 'd3dso', 'd3dtess', 'd3dmrt', 'd3dcopysrgb', 'd3dcbdyn', 'd3dvbring', 'd3dfence') {
  $f0 = $fail
  Run-Test (Join-Path $Vd "$t.exe")
  if ($t -eq 'd3dsm5' -and $fail -gt $f0) { foreach ($k in 1..3) { Run-Test (Join-Path $Vd 'd3dsm5.exe') @('bits') "d3dsm5 bits #$k" } }
}
Run-Test (Join-Path $Vd 'd3dbc.exe') @('lossy') 'd3dbc lossy'
$gt = @((Join-Path $PSScriptRoot 'tools\gputest.exe'), (Join-Path $Vd 'gputest.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($gt) { Run-Test $gt } else { W '  MISS gputest.exe' ; $miss++ }

W '== 4. D3D11 tests (x86, 32-bit UMD)'
foreach ($t in 'd3dcaps32', 'd3dmsaa32', 'd3dcs32', 'd3dcbdyn32', 'd3dvbring32') { Run-Test (Join-Path $Vd "x86\$t.exe") }

W '== 5. OpenGL'
Run-Test (Join-Path $Vd 'gltest.exe')

W '== 6. timing'
Run-Test (Join-Path $Vd 'vblank.exe') @('120')
Run-Test (Join-Path $Vd 'dwmrate.exe') @('5')
Run-Test (Join-Path $Vd 'gpuclock.exe') @('1000', '3')
$ks = @((Join-Path $PSScriptRoot 'tools\kmdstats.exe'), (Join-Path $PSScriptRoot 'out-gpu\tools\kmdstats.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($ks) { & $ks 2 2>&1 | Select-Object -First 8 | ForEach-Object { W "  kmd> $_" } }

if ($TrySwapped.Count -and -not $Keep) { W '== TryUmd: putting the installed UMD back'; Swap-Back }
elseif ($TrySwapped.Count) { W '== TryUmd -Keep: the new UMD stays (reboot so DWM loads it; rebuild the kit package with it)' }

W '== 7. crashes since this boot'
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1002; StartTime = $boot } -ErrorAction SilentlyContinue
$ev | Select-Object -First 15 | ForEach-Object { W ("  {0:HH:mm:ss} id {1}: {2}" -f $_.TimeCreated, $_.Id, (($_.Message -split "`n")[0..1] -join ' ').Trim()) }
W "  application errors/hangs since boot: $(@($ev).Count)"
$tdr = Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; StartTime = $boot } -ErrorAction SilentlyContinue
W "  Display (TDR 4101) events since boot: $(@($tdr).Count)"
$dwm = Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dwm) { W ("  dwm#{0} up {1:N0} s (boot {2:N0} s ago)" -f $dwm.Id, ((Get-Date) - $dwm.StartTime).TotalSeconds, ((Get-Date) - $boot).TotalSeconds) }

W ''
W "SUMMARY: $pass PASS, $fail FAIL/HANG, $nKnown known, $miss missing   -> $Report"
Copy-Item $Report $Dir -Force
if (Test-Path 'C:\S10Prov\gputest') { Copy-Item 'C:\S10Prov\gputest' (Join-Path $Dir 'gputest-bmp') -Recurse -Force -ErrorAction SilentlyContinue }
Remove-Item 'C:\S10Prov\gpu-validate.zip' -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$Dir\*" -DestinationPath 'C:\S10Prov\gpu-validate.zip' -ErrorAction SilentlyContinue
W 'send C:\S10Prov\gpu-validate.zip'
W 'By hand (write in the chat): Edge edge://gpu (Graphics Feature Status), a WebGL demo, YouTube 1080p, scrolling.'
