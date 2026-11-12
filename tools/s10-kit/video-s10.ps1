<#
  video-s10.ps1 (v1.0) - Galaxy S10: hardware H.264 video decode on the Exynos 9820 MFC (drivers/Exynos9820Mfc) and
  the GPU user-mode driver's D3D11 video decoder on top of it (UMD 0222: Video.cpp, notes/g17-dxva-mfc.md,
  notes/g35-browser-video.md). Run ON THE PHONE, PowerShell as Administrator, from the kit folder:
      Set-ExecutionPolicy -Scope Process Bypass
      .\video-s10.ps1 install   adds + starts out\Exynos9820Mfc (built by build-s10.cmd v1.14 when mfc\mfc_fw.bin was
                                there - get-wifi-fw.cmd v1.2 pulls it from the phone in TWRP). No reboot needed.
      .\video-s10.ps1 test      (default) the decode checks, all against libavcodec's decode of the same clips
                                (video\*.ref.txt, FNV-1a 32 per frame):
                                  1 device ACPI\SAMS982A\0 started, driver + firmware file in place
                                  2 mfcdec  video\mfc_test.h264   - the MFC directly (driver IOCTLs), 3 known frames
                                  3 mfcdec  video\c1080.h264      - 1080p High, B-frames: 90 frames bit-exact + fps
                                  4 d3dvdec                       - the UMD offers the H.264 DXVA decoder
                                  5 mftest  c1080 / c720 (Media Foundation, -d3d11 = DXVA on the MFC, the path apps
                                    and players take): bit-exact, DXVA output texture, UMD "decoded N" line + fps;
                                    the same clip through Microsoft's software decoder for the speed comparison
                                  6 the same DXVA decode inside an AppContainer (packaged apps: Media Player, Photos)
      .\video-s10.ps1 play      opens video\play.mp4 (1080p30, 20 s) in the default player, then in Edge; while each
                                plays, a one-session probe on the MFC tells whether that player holds it (= hardware
                                decode) and the player's CPU use is measured. Close nothing - the script does it.
      .\video-s10.ps1 all       test, then play.
  Everything is logged to C:\S10Prov\video-s10.txt - send that file back.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'test', 'play', 'all')][string]$Action = 'test')
$ErrorActionPreference = 'Continue'
$Out = 'C:\S10Prov\video-s10.txt'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
if ($Action -ne 'play') { "video-s10.ps1 v1.0  $Action  $(Get-Date -Format s)" | Set-Content $Out -Encoding ascii }
else { "" | Add-Content $Out -Encoding ascii; "video-s10.ps1 v1.0  $Action  $(Get-Date -Format s)" | Add-Content $Out -Encoding ascii }
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }
function Ok($m) { Write-Host "[ OK ] $m" -ForegroundColor Green; "[ OK ] $m" | Add-Content $Out -Encoding ascii }
function Bad($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; "[FAIL] $m" | Add-Content $Out -Encoding ascii; $script:Fails++ }
function Wn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow; "[WARN] $m" | Add-Content $Out -Encoding ascii }
$script:Fails = 0
$K = $PSScriptRoot
$Media = Join-Path $K 'video'
$Tools = Join-Path $K 'tools\video'
$DevId = 'ACPI\SAMS982A\0'
$Work = 'C:\S10Prov\video-work'          # tools + clips + logs readable by AppContainers
New-Item -ItemType Directory -Force $Work | Out-Null

function Hashes($lines) {
  @($lines | ForEach-Object { if ("$_" -match 'luma fnv (0x[0-9a-f]+) chroma fnv (0x[0-9a-f]+)') { "$($Matches[1])/$($Matches[2])" } })
}
function Compare-Ref($name, $got, $refFile) {
  $ref = Hashes (Get-Content $refFile)
  $n = [Math]::Min($got.Count, $ref.Count); $same = 0; $first = -1
  for ($i = 0; $i -lt $n; $i++) { if ($got[$i] -eq $ref[$i]) { $same++ } elseif ($first -lt 0) { $first = $i } }
  $msg = "{0}: {1} frames, {2}/{3} identical to libavcodec" -f $name, $got.Count, $same, $ref.Count
  if ($got.Count -eq $ref.Count -and $same -eq $ref.Count) { Ok $msg }
  else { Bad ("$msg" + $(if ($first -ge 0) { ", first difference at frame $first ($($got[$first]) vs $($ref[$first]))" } else { '' })) }
}
function Device {
  $d = Get-PnpDevice -InstanceId $DevId -ErrorAction SilentlyContinue
  if (-not $d) { return $null }
  $p = Get-PnpDeviceProperty -InstanceId $DevId -KeyName 'DEVPKEY_Device_ProblemCode', 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue
  [pscustomobject]@{ Status = $d.Status; Problem = ($p | Where-Object KeyName -eq 'DEVPKEY_Device_ProblemCode').Data
                     Version = ($p | Where-Object KeyName -eq 'DEVPKEY_Device_DriverVersion').Data
                     Inf = ($p | Where-Object KeyName -eq 'DEVPKEY_Device_DriverInfPath').Data; Name = $d.FriendlyName }
}
function Stage {
  # tools + media into one folder that ALL APPLICATION PACKAGES may read (acrun) and the UMD log folder they may write
  foreach ($f in 'mfcdec.exe', 'mftest.exe', 'd3dvdec.exe', 'acrun.exe') {
    if (Test-Path (Join-Path $Tools $f)) { Copy-Item (Join-Path $Tools $f) $Work -Force } else { Wn "tools\video\$f missing" }
  }
  foreach ($f in 'mfc_test.h264', 'mfc_test.ref.txt', 'c1080.h264', 'c1080.mp4', 'c1080.ref.txt', 'c720.mp4', 'c720.ref.txt', 'play.mp4') {
    if (Test-Path (Join-Path $Media $f)) { Copy-Item (Join-Path $Media $f) $Work -Force } else { Wn "video\$f missing" }
  }
  New-Item -ItemType Directory -Force "$Work\tlog" | Out-Null
  icacls $Work /grant '*S-1-15-2-1:(OI)(CI)RX' /grant '*S-1-5-11:(OI)(CI)RX' /T /Q | Out-Null
  icacls "$Work\tlog" /grant '*S-1-15-2-1:(OI)(CI)M' /grant '*S-1-5-11:(OI)(CI)M' /T /Q | Out-Null
}
function UmdVideoLog($exeName) {
  $f = Get-ChildItem "$Work\tlog" -Filter "malikm-$exeName*" -ErrorAction SilentlyContinue | Where-Object Name -notlike '*stderr*' |
       Sort-Object LastWriteTime | Select-Object -Last 1
  if (-not $f) { W "    (no UMD log from $exeName)"; return $null }
  $l = Get-Content $f.FullName | Select-String 'video: (MFC|decoder|stream not|frame \d+: (no|IOCTL)|.*failed)' | ForEach-Object { $_.Line -replace '\s+', ' ' }
  $l | Select-Object -First 12 | ForEach-Object { W "    umd: $_" }
  return ($l -join "`n")
}

# ------------------------------------------------------------------------------------------------------------- install
if ($Action -eq 'install') {
  $inf = $null
  foreach ($c in (Join-Path $K 'out\Exynos9820Mfc\Exynos9820Mfc.inf'), (Join-Path $K 'Exynos9820Mfc\Exynos9820Mfc.inf')) { if (Test-Path $c) { $inf = $c; break } }
  if (-not $inf) { Bad 'no out\Exynos9820Mfc\Exynos9820Mfc.inf - run get-wifi-fw.cmd (v1.2, TWRP) and build-s10.cmd (v1.14) on the PC first'; exit 1 }
  $pk = Split-Path $inf
  foreach ($f in 'Exynos9820Mfc.sys', 'mfc_fw.bin', 'mfc_test.h264', 'exynos9820mfc.cat') {
    if (-not (Test-Path (Join-Path $pk $f))) { Bad "package file $f missing in $pk"; exit 1 }
  }
  W ("package: {0}, mfc_fw.bin {1} B" -f $pk, (Get-Item (Join-Path $pk 'mfc_fw.bin')).Length)
  $d0 = Device
  if (-not $d0) { Bad "$DevId is not present: the UEFI shows the MFC (DSDT VIDC) only when it found its power domain on - send this file"; exit 1 }
  W "before: $($d0.Status) problem $($d0.Problem) driver $($d0.Version) $($d0.Inf)"
  pnputil /add-driver $inf /install 2>&1 | ForEach-Object { W "  pnputil: $_" }
  for ($i = 0; $i -lt 20; $i++) { Start-Sleep -Milliseconds 500; $d = Device; if ($d.Status -eq 'OK') { break } }
  W "after: $($d.Status) problem $($d.Problem) driver $($d.Version) $($d.Inf)"
  if ($d.Status -eq 'OK') { Ok "MFC driver started ($($d.Name)) - now: .\video-s10.ps1 test" }
  else { Bad "the device did not start (problem $($d.Problem): 10 = the firmware did not boot / answer, 28 = no driver) - send $Out" }
  exit 0
}

# ------------------------------------------------------------------------------------------------------------------ test
if ($Action -in 'test', 'all') {
  Stage
  W ''
  W '== 1 device'
  $d = Device
  if (-not $d) { Bad "$DevId not present (UEFI did not see the MFC power domain on)" }
  elseif ($d.Status -ne 'OK') { Bad "$DevId $($d.Status), problem $($d.Problem), driver '$($d.Version)' - .\video-s10.ps1 install" }
  else { Ok "$DevId started, driver $($d.Version) ($($d.Inf))" }
  foreach ($f in 'Exynos9820Mfc.sys', 'mfc_fw.bin', 'mfc_test.h264') {
    $p = "$env:SystemRoot\System32\drivers\$f"
    if (Test-Path $p) { W ("  {0} {1} B" -f $f, (Get-Item $p).Length) } else { Wn "$p missing" }
  }
  $umd = "$env:SystemRoot\System32\maliumd.dll"
  if (Test-Path $umd) { W ("  maliumd.dll sha256 {0}" -f (Get-FileHash $umd).Hash.ToLower().Substring(0, 8)) }
  $mfcdec = "$Work\mfcdec.exe"; $mftest = "$Work\mftest.exe"

  W ''
  W '== 2 mfcdec mfc_test.h264 (the MFC through the driver IOCTLs)'
  $o = & $mfcdec "$Work\mfc_test.h264" 2>&1 | ForEach-Object { "$_" }
  $o | Select-Object -Last 5 | ForEach-Object { W "    $_" }
  Compare-Ref 'mfc_test' (Hashes $o) "$Work\mfc_test.ref.txt"

  W ''
  W '== 3 mfcdec c1080.h264 (1080p High, 3 B-frames, 4 refs)'
  $o = & $mfcdec "$Work\c1080.h264" 2>&1 | ForEach-Object { "$_" }
  $o | Where-Object { $_ -notmatch '^frame' } | ForEach-Object { W "    $_" }
  Compare-Ref 'c1080 (MFC)' (Hashes $o) "$Work\c1080.ref.txt"

  W ''
  W '== 4 d3dvdec (the UMD video decoder: profiles, NV12, decoder object)'
  $o = & "$Work\d3dvdec.exe" 2>&1 | ForEach-Object { "$_" }
  $o | ForEach-Object { W "    $_" }
  if ($o -contains 'PASS') { Ok 'd3dvdec PASS' } else { Bad 'd3dvdec did not PASS' }

  W ''
  W '== 5 Media Foundation (mftest): software decoder vs -d3d11 (DXVA on the MFC)'
  foreach ($c in @(@{ N = 'c1080'; R = 1080 }, @{ N = 'c720'; R = 720 })) {
    $clip = "$Work\$($c.N).mp4"
    $sw = & $mftest $clip -rows $c.R 2>&1 | ForEach-Object { "$_" }
    W ("  software: {0}" -f (($sw | Where-Object { $_ -match '^done' }) -join ' '))
    Get-ChildItem "$Work\tlog" -Filter 'malikm-mftest*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    $env:MALIKM_TGSI_DIR = "$Work\tlog"
    $hw = & $mftest $clip -d3d11 -rows $c.R 2>&1 | ForEach-Object { "$_" }
    Remove-Item Env:MALIKM_TGSI_DIR -ErrorAction SilentlyContinue
    $hw | Where-Object { $_ -notmatch '^frame' } | ForEach-Object { W "  d3d11: $_" }
    $log = UmdVideoLog 'mftest'
    # the software run is only the speed baseline (Microsoft's decoder may be missing, e.g. N editions): no FAIL
    if ((Hashes $sw).Count) { Compare-Ref "$($c.N) software decoder" (Hashes $sw) "$Work\$($c.N).ref.txt" }
    else { Wn "$($c.N): no software decode ($(($sw | Select-Object -Last 1)))" }
    Compare-Ref "$($c.N) DXVA" (Hashes $hw) "$Work\$($c.N).ref.txt"
    $tex = ($hw | Where-Object { $_ -match 'output texture' }) -join ''
    if ($tex -match 'bind 0x([0-9a-f]+)' -and ([Convert]::ToInt32($Matches[1], 16) -band 0x200)) { Ok "$($c.N): output is a DXVA decoder texture" }
    else { Bad "$($c.N): Media Foundation did not use DXVA ($tex)" }
    if ($log -match 'decoded (\d+), failed (\d+)') { if ([int]$Matches[2] -eq 0 -and [int]$Matches[1] -gt 0) { Ok "$($c.N): UMD decoded $($Matches[1]) frames on the MFC, 0 failed" } else { Bad "$($c.N): UMD decoded $($Matches[1]), failed $($Matches[2])" } }
  }

  W ''
  W '== 6 AppContainer (the sandbox of Media Player / Photos): mftest -d3d11 c720 through acrun'
  Get-ChildItem "$Work\tlog" -Filter 'malikm-mftest*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
  $env:MALIKM_TGSI_DIR = "$Work\tlog"
  $ac = & "$Work\acrun.exe" "$Work\mftest.exe" "$Work\c720.mp4" -d3d11 -rows 720 2>&1 | ForEach-Object { "$_" }
  Remove-Item Env:MALIKM_TGSI_DIR -ErrorAction SilentlyContinue
  $ac | Where-Object { $_ -notmatch '^frame' } | ForEach-Object { W "  ac: $_" }
  $log = UmdVideoLog 'mftest'
  if ($ac -match 'CreateProcess in the AppContainer|0xC0000142') { Wn 'the AppContainer could not start from this session (run it from the desktop PowerShell window, not over SSH)' }
  else {
    Compare-Ref 'c720 DXVA in an AppContainer' (Hashes $ac) "$Work\c720.ref.txt"
    if ($log -match 'MFC open failed') { Bad 'AppContainer: the UMD could not open the MFC device (ACL)' }
  }
  W ''
  if ($script:Fails -eq 0) { Ok 'all decode checks passed' } else { W "[FAIL] $($script:Fails) check(s) failed" }
}

# ------------------------------------------------------------------------------------------------------------------ play
if ($Action -in 'play', 'all') {
  if (-not (Test-Path "$Work\play.mp4")) { Stage }
  $clip = "$Work\play.mp4"
  function Probe {
    # one MFC session device-wide: a player decoding on the MFC holds it -> IOCTL_MFC_OPEN fails with 170 (busy)
    $o = & "$Work\mfcdec.exe" "$Work\mfc_test.h264" -q 2>&1 | ForEach-Object { "$_" }
    if ($o -match 'IOCTL_MFC_OPEN: 170') { return 'BUSY (the player decodes on the MFC)' }
    if ($o -match '^done: 3 frames') { return 'FREE (the player does NOT use the MFC)' }
    return "? ($($o -join ' | '))"
  }
  function CpuOf($names, $secs) {
    $a = @{}; Get-Process -Name $names -ErrorAction SilentlyContinue | ForEach-Object { $a[$_.Id] = $_.CPU }
    Start-Sleep -Seconds $secs
    $sum = 0.0; Get-Process -Name $names -ErrorAction SilentlyContinue | ForEach-Object { if ($a.ContainsKey($_.Id)) { $sum += $_.CPU - $a[$_.Id] } }
    '{0:N0} % of one core' -f (100 * $sum / $secs)
  }
  function Top($secs) {
    $a = @{}; Get-Process | ForEach-Object { $a[$_.Id] = $_.CPU }
    Start-Sleep -Seconds $secs
    $d = foreach ($p in Get-Process) { if ($null -ne $p.CPU -and $a.ContainsKey($p.Id)) { [pscustomobject]@{ N = "$($p.Name)"; D = $p.CPU - $a[$p.Id] } } }
    ($d | Sort-Object D -Descending | Select-Object -First 5 | ForEach-Object { '{0} {1:N0}%' -f $_.N, (100 * $_.D / $secs) }) -join ', '
  }
  W ''
  W "== play: before - MFC $(Probe)"
  # 1) the default player (Media Player). explorer starts it in the user session, not elevated.
  $assoc = (cmd /c assoc .mp4) -join ' '
  W "  .mp4 opens with: $assoc"
  $before = @(Get-Process | ForEach-Object Id)
  Start-Process explorer.exe -ArgumentList "`"$clip`""
  Start-Sleep -Seconds 7
  $new = Get-Process | Where-Object { $before -notcontains $_.Id } | Select-Object -ExpandProperty Name -Unique
  W "  new processes: $($new -join ', ')"
  W "  default player: MFC $(Probe)"
  W "  CPU (8 s, top 5): $(Top 8)"
  $players = @($new | Where-Object { $_ -match 'Media|Video|wmplayer|Photos|mpv|vlc' })
  if ($players) { W "  player CPU ($($players -join ',')): $(CpuOf $players 5)" }
  foreach ($p in $players) { Get-Process -Name $p -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Seconds 2
  # 2) Edge (Chromium: D3D11VideoDecoder needs FL 11_0 + the UMD's ID3D11VideoDevice1)
  # a page with the clip (muted autoplay is allowed), opened through explorer: the user's browser, not elevated
  $edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($edge) {
    $page = "$Work\play.html"
    '<!doctype html><title>S10 video</title><body style="margin:0;background:#000"><video src="play.mp4" autoplay muted loop controls style="width:100%"></video>' |
      Set-Content $page -Encoding ascii
    Start-Process explorer.exe -ArgumentList "`"microsoft-edge:file:///$($page -replace '\\', '/')`""
    Start-Sleep -Seconds 10
    if (-not (Get-Process msedge -ErrorAction SilentlyContinue)) { Start-Process $edge -ArgumentList "file:///$($page -replace '\\', '/')"; Start-Sleep -Seconds 10 }
    W "  Edge: MFC $(Probe)"
    W "  CPU (8 s, top 5): $(Top 8)"
    W "  Edge CPU (all msedge processes): $(CpuOf 'msedge' 5)"
    W '  (in Edge, edge://media-internals -> the clip -> kVideoDecoderName: D3D11VideoDecoder = hardware, FFmpegVideoDecoder = software)'
  } else { Wn 'Edge not found' }
  W ''
  W 'play done: if a probe said BUSY, that player decodes on the MFC. Close Edge yourself when done looking.'
}
W "log: $Out"
