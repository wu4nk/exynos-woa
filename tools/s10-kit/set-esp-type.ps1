<#
  set-esp-type.ps1 (v1.3-s10) - phone in TWRP, USB cable in. Galaxy S10 (beyond1lte) edition
  of the z3s script: refuses any phone whose bootloader is not G973*, and checks that
  cache / userdata still carry the GPT GUIDs the BCD was built for, before writing.

  Windows Setup finds its system partition (ESP) by the GPT type GUID. Samsung
  types every partition "basic data", so the specialize pass stops with
  "Windows Setup could not configure Windows to run on this computer's hardware".
  This marks `cache` (the Windows boot partition) as an EFI system partition in
  BOTH copies of the UFS LUN 0 GPT - the same fix the S9+ port uses. Only the
  16-byte type GUID of that one entry and the two CRCs change.

    - the original sectors are saved to C:\s10\gpt-backup first,
    - every write is read back; on any difference the original goes back,
    - -Restore writes the newest saved originals back.
    - Before touching the GPT it tries to copy C:\Windows\Panther\setuperr.log /
      setupact.log off userdata (only if this TWRP can mount NTFS).

  PowerShell (no admin needed), z3s-disk.cs next to this file, adb in PATH:
    .\set-esp-type.ps1
  Then run flash-windows.cmd again (Setup has to start from a clean image).
#>
param(
  [string] $OutDir = 'C:\s10',
  # GPT unique GUIDs of THIS phone's cache (sda31) and userdata (sda34), read 2026-11-02;
  # make-windows.ps1 builds the BCD on them. Another S10 has other values.
  [string] $EspGuid = '21ca1362-c5e9-45d2-b5bd-3f4dd0f30251',
  [string] $WinGuid = '712bcd12-3071-4f1c-8951-9c75a5ecca08',
  [long]   $UserdataLastLba = 31236090,
  [switch] $Restore,
  [switch] $FromFlash    # called by flash-windows.cmd: no "run flash-windows.cmd" hint
)
$ErrorActionPreference = 'Stop'
function Ok($m)   { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }
Write-Host 'set-esp-type v1.3-s10'

$cs = Join-Path $PSScriptRoot 'z3s-disk.cs'
if (-not (Test-Path $cs)) { Fail 'z3s-disk.cs missing next to this script' }
if (-not ('Z3s.GptType' -as [type])) { Add-Type -TypeDefinition (Get-Content -Raw $cs) }
if (-not ('Z3s.GptRepair' -as [type])) { Fail 'an older z3s-disk.cs is loaded in this PowerShell window - close it and open a new one' }

$AdbExe = (Get-Command adb -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not $AdbExe) { Fail 'adb not found in PATH (platform-tools)' }
function Invoke-Adb {
  $ErrorActionPreference = 'Continue'
  $o = & $AdbExe @args 2>&1 | ForEach-Object { "$_" }
  return @{ Code = $LASTEXITCODE; Out = ($o -join "`n").Trim() }
}
function Sh([string]$cmd) { return (Invoke-Adb shell $cmd).Out }

$st = Invoke-Adb get-state
if ($st.Code -or $st.Out -notmatch 'recovery') { Fail "adb: '$($st.Out)' - boot TWRP and plug the cable" }

$bl = Sh 'getprop ro.boot.bootloader'
if ($bl -notmatch '^G973') { Fail "this phone's bootloader is '$bl' - this kit is for the Galaxy S10 (G973*) only. Nothing written." }
Ok "Galaxy S10, bootloader $bl"
$node = Sh 'readlink -f /dev/block/by-name/cache'
if ($node -notmatch '^/dev/block/(sd[a-z])[0-9]+$') { Fail "cache resolves to '$node'" }
$unit = $Matches[1]
$disk = "/dev/block/$unit"
$ss   = [int](Sh "cat /sys/block/$unit/queue/logical_block_size")
$sz   = [long](Sh "cat /sys/block/$unit/size")
if ($ss -ne 4096) { Fail "$disk has $ss-byte sectors, expected 4096" }
$last = [long]($sz * 512 / $ss) - 1
Ok "cache = $node on $disk, last LBA $last"

$tmpL = Join-Path $env:TEMP 'z3s-gpt.bin'
function Read-Lba([long]$start, [int]$count) {
  # v1.2: delete the old temp file first and check the size on the phone, so a
  # dd that fails quietly (adbd without exit codes) cannot hand us stale data.
  $r = Invoke-Adb shell "rm -f /tmp/z3s-gpt.bin; dd if=$disk bs=$ss skip=$start count=$count of=/tmp/z3s-gpt.bin 2>&1; sync; stat -c %s /tmp/z3s-gpt.bin 2>/dev/null"
  $want = $count * $ss
  if ($r.Out -notmatch "(?m)^$want\s*$") { Fail "read $disk LBA $start x$count failed on the phone: $($r.Out)" }
  if (Test-Path $tmpL) { Remove-Item $tmpL -Force }
  $r = Invoke-Adb pull /tmp/z3s-gpt.bin $tmpL
  if ($r.Code -or -not (Test-Path $tmpL)) { Fail "adb pull: $($r.Out)" }
  $b = [IO.File]::ReadAllBytes($tmpL)
  if ($b.Length -ne $want) { Fail "read $disk LBA $start : got $($b.Length) bytes" }
  return ,$b
}
function Write-Lba([long]$start, [byte[]]$data) {
  [IO.File]::WriteAllBytes($tmpL, $data)
  $r = Invoke-Adb push $tmpL /tmp/z3s-gpt-w.bin
  if ($r.Code) { Fail "adb push: $($r.Out)" }
  $r = Invoke-Adb shell "dd if=/tmp/z3s-gpt-w.bin of=$disk bs=$ss seek=$start conv=fsync 2>/dev/null; sync"
  $back = Read-Lba $start ($data.Length / $ss)
  return ([Convert]::ToBase64String($back) -eq [Convert]::ToBase64String($data))
}

# ---------------------------------------------------------- the two regions
$h1 = Read-Lba 1 1
$n = [BitConverter]::ToUInt32($h1, 80); $esz = [BitConverter]::ToUInt32($h1, 84)
$entSec = [int][math]::Ceiling($n * $esz / $ss)
if ([BitConverter]::ToInt64($h1, 72) -ne 2 -or [BitConverter]::ToInt64($h1, 32) -ne $last) { Fail 'primary GPT is not laid out as expected (entries at LBA 2, backup at the last LBA)' }
# S10: the BCD names partitions by GUID, and this phone's cache/userdata GUIDs are random
# ones (not Samsung's name-derived), so check them before anything is written.
$ents = Read-Lba 2 $entSec
$seen = @{}
for ($i = 0; $i -lt $n; $i++) {
  $o = $i * $esz
  $nm = [Text.Encoding]::Unicode.GetString($ents, $o + 56, 72).TrimEnd([char]0)
  if ($nm -eq 'cache' -or $nm -eq 'userdata') {
    $g = New-Object byte[] 16; [Array]::Copy($ents, $o + 16, $g, 0, 16)
    $seen[$nm] = ([guid]::new([byte[]]$g)).ToString()
  }
}
if ($seen['cache'] -ne $EspGuid -or $seen['userdata'] -ne $WinGuid) {
  Fail ("GPT GUIDs differ from the ones the Windows image was built for: cache $($seen['cache']) (want $EspGuid), userdata $($seen['userdata']) (want $WinGuid). " +
        "Rebuild with make-windows.ps1 -EspGuidText/-WinGuidText set to this phone's values. Nothing written.")
}
Ok "cache / userdata GUIDs match the image ($EspGuid / $WinGuid)"
$hb = Read-Lba $last 1
$bk = Join-Path $OutDir 'gpt-backup'
New-Item -ItemType Directory -Force $bk | Out-Null
if ([Text.Encoding]::ASCII.GetString($hb, 0, 8) -ne 'EFI PART') {
  # v1.3: the backup GPT is gone (seen 06/10: last LBA all zeros). Keep evidence of the
  # disk's tail, then rebuild the backup from the (CRC-checked) primary, like gdisk does.
  Write-Host '[WARN] backup GPT header at the last LBA is missing - looking at the end of the disk' -ForegroundColor Yellow
  $tailStart = $last - 2047
  $tail = Read-Lba $tailStart 2048
  $stamp0 = Get-Date -Format 'yyyyMMdd-HHmmss'
  [IO.File]::WriteAllBytes((Join-Path $bk "tail-$unit-lba$tailStart-$stamp0.bin"), $tail)
  $runs = @(); $cur = $null
  for ($i = 0; $i -lt 2048; $i++) {
    $z = $true
    for ($k = 0; $k -lt $ss; $k += 8) { if ([BitConverter]::ToUInt64($tail, $i * $ss + $k) -ne 0) { $z = $false; break } }
    $kind = if ($z) { 'zero' } else { 'data' }
    if ($cur -and $cur.Kind -eq $kind) { $cur.Last = $tailStart + $i } else { $cur = [pscustomobject]@{ Kind = $kind; First = $tailStart + $i; Last = $tailStart + $i }; $runs += $cur }
  }
  Write-Host "       last 2048 LBAs ($tailStart..$last):"
  foreach ($x in $runs) { Write-Host ("         {0} {1}..{2}" -f $x.Kind, $x.First, $x.Last) }
  $nbOff = [int](($UserdataLastLba - $tailStart) * $ss)
  $nbSig = [Text.Encoding]::ASCII.GetString($tail, $nbOff + 3, 4)
  Write-Host ("       userdata last sector (NTFS backup boot sector) {0}" -f $(if ($nbSig -eq 'NTFS') { 'is intact' } else { 'is NOT there' }))
  $prim = Read-Lba 1 (1 + $entSec)
  try { $nbk = [Z3s.GptRepair]::BackupFromPrimary($prim, $ss, $last) } catch { Fail "cannot rebuild the backup GPT: $($_.Exception.Message) - nothing written" }
  if (-not (Write-Lba ($last - $entSec) $nbk)) { Fail 'rebuilt backup GPT did not read back - nothing else written, run again' }
  Ok 'backup GPT rebuilt from the primary'
  $hb = Read-Lba $last 1
}
$bSig = [Text.Encoding]::ASCII.GetString($hb, 0, 8)
$bMy = [BitConverter]::ToInt64($hb, 24); $bAlt = [BitConverter]::ToInt64($hb, 32); $bEnt = [BitConverter]::ToInt64($hb, 72)
$diag = "backup header @LBA $last : sig '$($bSig -replace '[^ -~]', '.')' MyLBA $bMy AltLBA $bAlt EntriesLBA $bEnt (expected $($last - $entSec)); primary: entries $n x $esz"
if ($bSig -ne 'EFI PART') { Fail "no GPT header at the last LBA. $diag" }
if ($bMy -ne $last -or $bEnt + $entSec -ne $last) { Fail "backup GPT is not laid out as expected. $diag" }
$regions = @(
  @{ Name = 'primary'; Start = 1L;    Count = 1 + $entSec; Header = 1L },
  @{ Name = 'backup';  Start = $bEnt; Count = $entSec + 1; Header = $last }
)

if ($Restore) {
  foreach ($r in $regions) {
    $f = Get-ChildItem $bk -Filter "gpt-$($r.Name)-$unit-lba$($r.Start)-*.bin" | Sort-Object Name | Select-Object -Last 1
    if (-not $f) { Fail "no saved $($r.Name) copy in $bk" }
    $data = [IO.File]::ReadAllBytes($f.FullName)
    if ($data.Length -ne $r.Count * $ss) { Fail "$($f.Name) has the wrong size" }
    if (-not (Write-Lba $r.Start $data)) { Fail "$($r.Name) GPT did not read back after restore - run again" }
    Ok "$($r.Name) GPT restored from $($f.Name)"
  }
  exit 0
}

# ------------------------------------------ Setup logs (best effort, read-only)
$logs = Join-Path $OutDir 'logs'
$m = Sh 'mkdir -p /tmp/win; umount /tmp/win 2>/dev/null; (mount -t ntfs3 -o ro /dev/block/by-name/userdata /tmp/win || mount -t ntfs -o ro /dev/block/by-name/userdata /tmp/win || ntfs-3g -o ro /dev/block/by-name/userdata /tmp/win) >/dev/null 2>&1; ls /tmp/win/Windows/Panther 2>/dev/null'
if ($m -match 'setupact') {
  New-Item -ItemType Directory -Force $logs | Out-Null
  foreach ($f in 'setuperr.log', 'setupact.log', 'UnattendGC/setupact.log') {
    Invoke-Adb pull "/tmp/win/Windows/Panther/$f" (Join-Path $logs ($f -replace '/', '-')) | Out-Null
  }
  Invoke-Adb pull '/tmp/win/Windows/INF/setupapi.offline.log' (Join-Path $logs 'setupapi.offline.log') | Out-Null
  Invoke-Adb pull '/tmp/win/Windows/INF/setupapi.dev.log' (Join-Path $logs 'setupapi.dev.log') | Out-Null
  # a bugcheck during Setup leaves a minidump
  if ((Sh 'ls /tmp/win/Windows/Minidump 2>/dev/null') -match '\.dmp') {
    Invoke-Adb pull '/tmp/win/Windows/Minidump' (Join-Path $logs 'Minidump') | Out-Null
    Warn "there are crash dumps (BSOD) - copied to $logs\Minidump"
  }
  if ((Sh 'ls /tmp/win/Windows/MEMORY.DMP 2>/dev/null') -match 'MEMORY') { Warn 'Windows\MEMORY.DMP exists (a BSOD happened); not copied, it is large' }
  Ok "Setup logs copied to $logs"
} else {
  Warn 'this TWRP cannot mount NTFS - no Setup logs (not needed for the fix)'
}
Sh 'umount /tmp/win 2>/dev/null' | Out-Null

# ------------------------------------------------------------- change type
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$changed = 0
foreach ($r in $regions) {
  $orig = Read-Lba $r.Start $r.Count
  $data = [byte[]]$orig.Clone()
  try {
    $did = [Z3s.GptType]::Set($data, $ss, $r.Start, $r.Header, 'cache', [Z3s.GptType]::Esp)
  } catch { Fail "$($r.Name) GPT: $($_.Exception.Message) - nothing written" }
  if (-not $did) { Ok "$($r.Name) GPT: cache is already an EFI system partition"; continue }
  $save = Join-Path $bk "gpt-$($r.Name)-$unit-lba$($r.Start)-$stamp.bin"
  [IO.File]::WriteAllBytes($save, $orig)
  if (-not (Write-Lba $r.Start $data)) {
    $back = Write-Lba $r.Start $orig
    Fail ("$($r.Name) GPT did not read back as written; " + ($(if ($back) { 'the original is back.' } else { "putting the original back ALSO failed - copy in $save, run with -Restore" })))
  }
  Ok "$($r.Name) GPT: cache is now an EFI system partition (original saved: $(Split-Path $save -Leaf))"
  $changed++
}
Sh 'rm -f /tmp/z3s-gpt.bin /tmp/z3s-gpt-w.bin' | Out-Null
Write-Host ''
if ($FromFlash) { Ok 'cache is an EFI system partition' } else { Ok 'done. Next: run flash-windows.cmd again (clean Windows image), then reboot.' }
