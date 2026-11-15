<#
  gpu-validate.ps1 (v1.5) - Galaxy S10: one run of every GPU check that needs no typing, after the upstream stack
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
  v1.4: -TryUmd after an earlier -Keep: the kept files' *.s10prev (the package's originals) stay untouched and the
  current files go to *.s10try for this run instead (2026-11-12 13:00: s10.4 was not swapped in because s10.3's
  -Keep left *.s10prev - the run tested s10.3). -Keep then leaves the replaced copy as *.s10try; -Restore still puts
  the originals (*.s10prev) back.
  v1.5 (2026-11-15, "fix xong gpu cho done"): gputest's texture-format-BC* differences count as KNOWN (BCn is
  re-compressed to ETC2/EAC on the Mali by design - upstream G26; d3dbc lossy checks those formats with the right
  tolerance) when they are its ONLY failures; any other FAIL / MISSING keeps it a FAIL. UMD 0222-s10.6 (Mesa s10-0011)
  fixes msaa4-resolve / msaa4-load-samples, so gputest is expected KNOWN with s10.6.
  -Soak <minutes>: endurance run instead of the checks - gputest + d3dcs + d3dtess + gltest in a loop at the
  installed clocks (806 MHz GPU on the kit's final setup) for that long, one line per round with the result and the
  S10ePower temperatures / GPU clock, then TDRs, application crashes, KMD counters and the thermal guard's peak.
  Writes C:\S10Prov\gpu-soak.txt. Stop early with Ctrl+C (the report so far stays).
  Not automatic (look yourself, write the result to the chat): Edge -> edge://gpu ("Hardware accelerated" lines),
  a WebGL page, a YouTube video, scrolling in Settings.
#>
param([int]$Timeout = 60, [string]$TryUmd = '', [switch]$Keep, [switch]$Restore, [int]$Soak = 0)
$ErrorActionPreference = 'Continue'
$Report = if ($Soak -gt 0) { 'C:\S10Prov\gpu-soak.txt' } else { 'C:\S10Prov\gpu-validate.txt' }
New-Item -ItemType Directory -Force (Split-Path $Report) | Out-Null
"gpu-validate.ps1 v1.5  $(Get-Date -Format s)  uptime $([int]([Environment]::TickCount / 1000)) s" | Set-Content $Report -Encoding ascii
function W($m) { Write-Host $m; $m | Add-Content $Report -Encoding ascii }
$Vd = Join-Path $PSScriptRoot 'tools\validate'
$pass = 0; $fail = 0; $nKnown = 0; $miss = 0
$Dir = 'C:\S10Prov\gpu-validate'; Remove-Item $Dir -Recurse -Force -ErrorAction SilentlyContinue
$Logs = Join-Path $Dir 'logs'; New-Item -ItemType Directory -Force $Logs | Out-Null
$env:MALIKM_TGSI_DIR = $Logs   # child processes inherit it: every test writes its UMD log there
$KnownFail = @{ 'd3dsm41.exe' = 'S10e: 7/8 (Bifrost LOD)' }
# v1.5: a test whose failures are ALL of a known kind (every FAIL / MISSING / BROKEN line matches) is KNOWN
$KnownLines = @{ 'gputest.exe' = @('^FAIL\s+texture-format-BC', 'BCn re-compressed to ETC2/EAC by design') }
function Known-Lines($name, $out) {
  if (-not $KnownLines.ContainsKey($name)) { return $false }
  $bad = @($out | Where-Object { "$_" -match '^(FAIL|MISSING|BROKEN)\s' })
  return ($bad.Count -gt 0 -and @($bad | Where-Object { "$_" -notmatch $KnownLines[$name][0] }).Count -eq 0)
}

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
  elseif ($KnownFail.ContainsKey($name)) { $res = 'KNOWN'; $why = $KnownFail[$name]; $script:nKnown++ }
  elseif (Known-Lines $name $out) { $res = 'KNOWN'; $why = $KnownLines[$name][1]; $script:nKnown++ }
  else { $res = 'FAIL'; $script:fail++ }
  $code = if ($ok) { $p.ExitCode } else { 'timeout' }
  W ("  {0,-5} {1,-22} exit {2}  {3} ms{4}" -f $res, $label, $code, $ms, $(if ($res -eq 'KNOWN') { "  ($why)" } else { '' }))
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
  if (Test-Path $prev) {          # v1.4: an earlier -Keep - its originals stay in .s10prev, this run backs up to .s10try
    $prev = "$dst.s10try"
    if (Test-Path $prev) { takeown /f $prev /a | Out-Null; icacls $prev /grant '*S-1-5-32-544:F' | Out-Null; Remove-Item $prev -Force -ErrorAction SilentlyContinue }
    if (Test-Path $prev) { W "  TryUmd: $prev cannot be removed (in use?) - $dst not swapped"; return }
  }
  try {
    Rename-Item $dst (Split-Path $prev -Leaf) -ErrorAction Stop
    Copy-Item $src $dst -Force -ErrorAction Stop
    $script:TrySwapped += ,@($dst, $prev)
    W ("  TryUmd: {0} <- {1} (sha256 {2})" -f $dst, $src, (Get-FileHash $dst).Hash.ToLower().Substring(0, 16))
  } catch {
    W "  TryUmd: $dst NOT swapped: $_"
    if (-not (Test-Path $dst) -and (Test-Path $prev)) { Rename-Item $prev (Split-Path $dst -Leaf) -ErrorAction SilentlyContinue }
  }
}
function Swap-Back {
  foreach ($pair in $script:TrySwapped) {
    $dst = $pair[0]; $bak = $pair[1]
    # renamed away, not deleted: a process started during the run may still have the new DLL loaded
    try {
      $old = "$dst.s10new"; if (Test-Path $old) { Remove-Item $old -Force -ErrorAction SilentlyContinue }
      if (Test-Path $old) { $old = "$dst.s10new$(Get-Random)" }
      Rename-Item $dst (Split-Path $old -Leaf) -ErrorAction Stop
      Rename-Item $bak (Split-Path $dst -Leaf) -ErrorAction Stop; W "  TryUmd: $dst restored (from $(Split-Path $bak -Leaf))"
    }
    catch { W "  TryUmd: RESTORE FAILED for $dst - rename $bak back by hand: $_" }
  }
}
if ($Restore) {
  W '== Restore: *.s10prev back in place'
  foreach ($dst in 'C:\Windows\System32\maliumd.dll', 'C:\Windows\SysWOW64\maliumd32.dll', 'C:\Windows\System32\maligl.dll') {
    if (-not (Test-Path "$dst.s10prev")) { W "  $dst : no .s10prev, left as is"; continue }
    foreach ($f in $dst, "$dst.s10prev") { if (Test-Path $f) { takeown /f $f /a | Out-Null; icacls $f /grant '*S-1-5-32-544:F' | Out-Null } }
    $script:TrySwapped = @(,@($dst, "$dst.s10prev")); Swap-Back
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

if ($Soak -gt 0) {
  # v1.5: endurance run. One line per round; a round = gputest (100 scenes vs WARP) + d3dcs + d3dtess + gltest.
  W "== soak: $Soak min at the installed clocks"
  $ctl = 'C:\S10Prov\power\s10epowerctl.exe'
  $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\S10ePower'
  function Pwr { if (Test-Path $ctl) { return ((& $ctl 2>&1 | Out-String) -replace '\s+', ' ').Trim() } return 'no s10epowerctl (S10ePower not installed)' }
  W "  start: $(Pwr)"
  $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
  $tdr0 = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; StartTime = $boot } -ErrorAction SilentlyContinue).Count
  $gt = @((Join-Path $PSScriptRoot 'tools\gputest.exe'), (Join-Path $Vd 'gputest.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
  $set = @()
  if ($gt) { $set += ,@($gt, @()) }
  foreach ($t in 'd3dcs', 'd3dtess', 'gltest') { $f = Join-Path $Vd "$t.exe"; if (Test-Path $f) { $set += ,@($f, @()) } }
  if (-not $set.Count) { W '  no test programs in the kit (tools\validate) - nothing to soak'; exit 1 }
  $end = (Get-Date).AddMinutes($Soak); $round = 0; $maxG = 0; $maxC = 0; $bad = 0
  while ((Get-Date) -lt $end) {
    $round++; $res = @()
    foreach ($e in $set) {
      $exe = $e[0]; $name = Split-Path $exe -Leaf; $o = Join-Path $env:TEMP "gs-$name.out"
      $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError "$o.err" -WorkingDirectory (Split-Path $exe)
      $null = $p.Handle
      if (-not $p.WaitForExit($Timeout * 1000)) { try { $p.Kill() } catch {}; $res += "$name HANG"; $bad++; continue }
      $out = @(Get-Content $o, "$o.err" -ErrorAction SilentlyContinue)
      if ($p.ExitCode -eq 0) { $res += "$name ok" }
      elseif (Known-Lines $name $out) { $res += "$name ok(BCn)" }
      else {
        $res += "$name FAIL($($p.ExitCode))"; $bad++
        $out | Set-Content (Join-Path $Dir "soak-r$round-$name.txt") -Encoding ascii
      }
    }
    $pw = Pwr
    if ($pw -match 'LITTLE (\d+) G3D (\d+)') { $maxC = [Math]::Max($maxC, [int]$Matches[1]); $maxG = [Math]::Max($maxG, [int]$Matches[2]) }
    $g3d = if ($pw -match 'G3D cur (\d+)') { "G3D $([int]$Matches[1] / 1000) MHz" } else { '' }
    $tp = if ($pw -match '(temp MID \d+ LITTLE \d+ G3D \d+)') { $Matches[1] } else { '' }
    $gd = if ($pw -match 'guard (\d+)') { "guard $($Matches[1])" } else { '' }
    W ("  {0:HH:mm:ss} round {1,3}: {2} | {3} {4} {5}" -f (Get-Date), $round, ($res -join ', '), $tp, $g3d, $gd)
  }
  $tdr = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; StartTime = $boot } -ErrorAction SilentlyContinue).Count - $tdr0
  $k = Get-ItemProperty $pk -ErrorAction SilentlyContinue
  W "  end:   $(Pwr)"
  W ("  thermal: peak during the soak LITTLE {0} / G3D {1}; driver peaks since boot TempPeakCpu {2} TempPeakG3d {3}, GuardTrips {4}, GuardPeak {5}" -f $maxC, $maxG, $k.TempPeakCpu, $k.TempPeakG3d, $k.GuardTrips, $k.GuardPeak)
  $ks = @((Join-Path $PSScriptRoot 'tools\kmdstats.exe'), (Join-Path $PSScriptRoot 'out-gpu\tools\kmdstats.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($ks) { & $ks 2 2>&1 | Select-Object -First 8 | ForEach-Object { W "  kmd> $_" } }
  $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1002; StartTime = $boot } -ErrorAction SilentlyContinue)
  $ev | Select-Object -First 10 | ForEach-Object { W ("  {0:HH:mm:ss} id {1}: {2}" -f $_.TimeCreated, $_.Id, (($_.Message -split "`n")[0]).Trim()) }
  if ($TrySwapped.Count -and -not $Keep) { Swap-Back }
  W ''
  W ("SOAK: {0} rounds in {1} min, {2} failed test runs, {3} TDR (display reset) events, {4} app crashes since boot -> {5}" -f $round, $Soak, $bad, $tdr, $ev.Count, $Report)
  W $(if ($bad -eq 0 -and $tdr -eq 0) { 'SOAK PASS' } else { 'SOAK FAIL - send C:\S10Prov\gpu-soak.txt (+ C:\S10Prov\gpu-validate\soak-*.txt)' })
  exit $(if ($bad -eq 0 -and $tdr -eq 0) { 0 } else { 1 })
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
