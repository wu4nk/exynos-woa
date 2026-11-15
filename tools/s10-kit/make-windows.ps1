<#
  make-windows.ps1 (v2.4.1-s10) - full Windows 11 ARM64 for the Galaxy S10 (beyond1lte,
  SM-G973F), built on the PC. The z3s (S20 Ultra) make-windows v2.1 with the S10's
  layout, drivers and UEFI; everything else - VHDX, lite, registry, unattend, BCD,
  pieces + flash-windows.cmd - is the same pipeline.

  S10 differences from the z3s script:
    - layout read from this phone's GPT (2026-11-02): cache sda31 LBA 3118592,
      userdata sda34 LBA 3286272, 114 482 458 624 B. userdata is NOT a whole number of
      MiB, so the NTFS volume is 109178 MiB and the last 251 LBAs of userdata stay
      unused (the export works in MiB); the flash script still checks the full size.
    - cache / userdata carry RANDOM GPT GUIDs on this phone (not Samsung's name-derived
      ones): -EspGuidText / -WinGuidText default to this phone's; set-esp-type.ps1 re-checks
      them on the phone before anything is written.
    - drivers: every package build-s10.cmd puts in out\ (Exynos9820Ufs -Board Beyond1, checked through
      its UFSBRD01 identity, Exynos9820Hsi2c, S10Touch, S10eBattery, Exynos9820WlanCx, Exynos9820Audio).
    - UEFI: newest beyond1lte-uefi-v*.img; flash-windows.cmd refuses a phone whose
      bootloader is not G973*.

  v2.4-s10 (2026-11-15, "a finished Windows, GPU final already"): the image comes up in the kit's FINISHED state, no
  gpu-s10.ps1 / power-s10.ps1 steps on the phone:
    - GPU: out-gpu\MaliG76Wddm (build-s10.cmd with S10_GPU=1: KMD 1.0.72.2 + UMD 0222-s10.4 in the package) is added
      to the image; TDR 10 / 20 s and GPU preemption off are written offline (what "gpu-s10.ps1 final" sets). The
      first boot (Setup / OOBE) runs it display-only - the proven path; SetupComplete.cmd then writes the final mode
      (FullAdapter 1, GpuEnable 1, Wddm2 5, DirectFlip 1) and the first logon restarts ONCE (20 s notice): from then
      the desktop is on the Mali, brightness works. No diagnostic task / variable is ever set. -NoGpu leaves it out.
    - power: S10ePower + S10eGovernor installed and started at boot (= "power-s10.ps1 enable") with the clocks this
      S10 passed its tests at: GPU 806 MHz (gpuoc), A55 2002 MHz (cpuoc) - -NoOc for stock clocks, -NoPower for none.
    - video: Exynos9820Mfc is in out\ (and so in the image) when build-s10.cmd found mfc\mfc_fw.bin.
    - the in-Windows scripts (gpu-s10, power-s10, panel-s10, video-s10, gpu-validate) + tools\ + video\ in C:\S10Prov\kit.
  v2.4.1-s10 (2026-11-15): S10eSpeakerTrim.cs in C:\S10Prov\kit (SetupComplete.cmd v1.3 turns the speakers on - the
  v2.8 image left them silent - a little louder, builds the speaker app, and cuts Windows' background work).
  Rescue as always: TWRP -> \S10EDIAG\nogpu.flg on the ESP hides the GPU for one boot.

  Same idea as the S9+ port: no repartitioning. Windows (NTFS) goes into
  `userdata`, `cache` becomes the FAT32 boot partition (ESP). The GPT stays as is.

    1. takes a Windows 11 ARM64 .iso (or install.wim / install.esd), picks the
       edition (Pro by default),
    2. builds a 4K-sector VHDX shaped like the phone: a 600 MiB FAT32 and a
       110868 MiB NTFS with the phone's disk GUID and cache / userdata
       partition GUIDs, so the BCD points straight at the phone's partitions,
    3. dism /Apply-Image, adds EVERY driver package under out\ (UFS, HSI2C, touch, battery,
       WiFi, audio, ...) and checks each one is in the image, WiFi firmware (bcm4375\) into
       System32\drivers, the whole kit into C:\S10Prov\kit (v2.3),
       trims inbox apps (unless -NoLite), registry: test signing, no fast startup,
       no VBS/HVCI, NoAutoMount (hides the 30 Android partitions), TPM / Secure Boot
       / RAM / CPU checks off for later upgrades, BypassNRO, scale 300 %, fewer effects,
    4. unattend.xml: local admin account, no Microsoft account, no network page,
    5. boot files + a BCD made with bcdedit /store (nothing touches this PC's
       own boot entries); its devices are set through the BCD WMI provider as the
       phone's GPT partitions (bcdedit would write vhd= devices),
    6. writes the used part of the NTFS + the whole ESP as files of <= 256 MiB
       (pieces\) and flash-windows.cmd: size check, cache typed as an EFI system
       partition in the GPT (set-esp-type.ps1; Setup looks for that type), adb + dd
       per file in TWRP, read-back SHA-256 of every file (a bad one is written
       again), UEFI to boot, reboot.

  Run after build-s10.cmd, in PowerShell **as Administrator**:
    cd <kit>\drivers
    Set-ExecutionPolicy -Scope Process Bypass
    .\make-windows.ps1 -Source D:\iso\22621.6060_arm64.iso
    .\make-windows.ps1 -Source D:\iso\x.iso -UserName Anh -Edition Home

  Needs about 40 GB free on the OutDir drive (default C:\s10). Output:
  C:\s10\windows\: pieces\ + uefi.img + flash-windows.cmd. Copy that
  folder anywhere, boot TWRP, double-click flash-windows.cmd. It does EVERYTHING in one go:
  cache -> ESP type, Windows, UEFI (boot partition), read-back checks, reboot.

  The UEFI goes into the same kit (newest beyond1lte-uefi-v*.img next to this script,
  in C:\s10, in the kit's out\ or in Downloads; or -Uefi <file>).
  If a run fails at step 7 or later, fix and add -Resume: work\windows.vhdx is reused.
#>
param(
  [Parameter(Mandatory = $true)] [string] $Source,
  [string] $Edition   = 'Pro',      # matched against the image names; ignored with -Index
  [int]    $Index     = 0,          # image index inside install.wim/esd (0 = pick by -Edition)
  [string] $OutDir    = 'C:\s10',
  [string] $DriverDir = (Join-Path $PSScriptRoot 'out'),
  [string] $UserName  = 's10',      # local admin account, no password
  [int]    $Scale     = 300,        # display scaling in %, for the 1440x3040 6.1" panel
  [switch] $NoLite,                 # keep every inbox app and service
  [switch] $Compact,                # dism /Compact: smaller image, more CPU on the phone
  [switch] $KeepVhd,                # keep work\windows.vhdx (about 15 GB) after export
  [switch] $Resume,                 # a run failed at step 7 or later: reuse work\windows.vhdx (skips 2-6)
  [string] $Uefi      = '',         # UEFI .img (beyond1lte-uefi-vX.Y.Z.img); default: newest one found
  [switch] $NoUefi,                 # do not put a UEFI into the flash kit
  [switch] $NoWifi,                 # v2.3: build without WiFi (otherwise Exynos9820WlanCx + bcm4375\ firmware are required)
  [string] $GpuDir    = (Join-Path $PSScriptRoot 'out-gpu'),   # v2.4: build-s10.cmd S10_GPU=1 output
  [switch] $NoGpu,                  # v2.4: no MaliG76Wddm in the image (Microsoft Basic Display)
  [switch] $NoPower,                # v2.4: no S10ePower / S10eGovernor
  [switch] $NoOc,                   # v2.4: stock clocks (no GPU 806 / A55 2002 MHz)
  # GPT unique GUIDs of THIS phone's cache (sda31) and userdata (sda34). The BCD points
  # at them. Another S10: read its GPT (entries 31 / 34, +16) and pass its values.
  [string] $EspGuidText = '21ca1362-c5e9-45d2-b5bd-3f4dd0f30251',
  [string] $WinGuidText = '712bcd12-3071-4f1c-8951-9c75a5ecca08'
)
$ScriptVersion = 'make-windows v2.4.1-s10'
$ErrorActionPreference = 'Stop'
function Ok($m)   { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Step($m) { Write-Host "`n==== $m ====" -ForegroundColor Cyan }
function Fail($m) { throw "[FAIL] $m" }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
      [Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Host '[FAIL] Run PowerShell as Administrator.' -ForegroundColor Red; exit 1
}

# ------------------------------------- phone layout (S10 live GPT, 2026-11-02)
$SS           = 4096
$EspBytes     = 629145600L          # cache (sda31),    LBA 3118592 - 3272191
$WinPartBytes = 114482458624L       # userdata (sda34), LBA 3286272 - 31236090 (whole partition)
$WinBytes     = [long][math]::Floor($WinPartBytes / 1MB) * 1MB   # NTFS volume: 109178 MiB (export works in MiB)
$EspLba       = 3118592
$WinLba       = 3286272
$DiskGuid     = [guid]'52444e41-494f-2044-4d4d-43204449534b'   # "ANDROID MMC DISK" (same as S10e / S9+)
$EspGuid      = [guid]$EspGuidText                             # cache    - random GUID on this phone
$WinGuid      = [guid]$WinGuidText                             # userdata - random GUID on this phone
$BasicData = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$EspOff    = 1MB
$WinOff    = 1MB + $EspBytes

if ($UserName -notmatch '^[A-Za-z0-9_.-]{1,20}$') { Write-Host '[FAIL] -UserName: letters, digits, _ . - only (max 20)' -ForegroundColor Red; exit 1 }
if ($Scale -lt 100 -or $Scale -gt 500)            { Write-Host '[FAIL] -Scale 100..500' -ForegroundColor Red; exit 1 }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$Work   = Join-Path $OutDir 'work'
$Dest   = Join-Path $OutDir 'windows'
$Vhd    = Join-Path $Work 'windows.vhdx'
Start-Transcript -Path (Join-Path $OutDir 'make-windows.log') -Force | Out-Null
Write-Host $ScriptVersion

$IsoPath = $null
$Hives   = New-Object System.Collections.Generic.List[string]

# Windows PowerShell 5.1 turns native stderr into terminating errors under
# 'Stop' when it is redirected, so native tools run with a local 'Continue'.
function Unload-Hives {
  $ErrorActionPreference = 'Continue'
  [gc]::Collect(); [gc]::WaitForPendingFinalizers()
  foreach ($h in @($Hives)) {
    for ($t = 0; $t -lt 5; $t++) {
      & reg.exe unload $h 2>$null | Out-Null
      if (-not $LASTEXITCODE) { break }
      Start-Sleep 2
    }
    if ($LASTEXITCODE) { Warn "could not unload $h - run: reg unload $h" }
  }
  $Hives.Clear()
}
function Load-Hive($name, $file) {
  $ErrorActionPreference = 'Continue'
  & reg.exe load "HKLM\$name" $file | Out-Null
  if ($LASTEXITCODE) { Fail "reg load $file" }
  $Hives.Add("HKLM\$name")
}
function RegSet($key, $name, $type, $data) {
  $ErrorActionPreference = 'Continue'
  & reg.exe add "HKLM\$key" /v $name /t $type /d $data /f | Out-Null
  if ($LASTEXITCODE) { Fail "reg add HKLM\$key /v $name" }
}
function Get-VhdDisk { Get-DiskImage -ImagePath $Vhd | Get-Disk }
function Letter($part) {
  $p = Get-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber
  if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) {
    $p | Add-PartitionAccessPath -AssignDriveLetter | Out-Null
    $p = Get-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber
  }
  if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) { Fail "no drive letter for partition $($p.PartitionNumber)" }
  return [string]$p.DriveLetter
}
function Is-Arm64Pe($file) {
  $b  = [IO.File]::ReadAllBytes($file)
  $pe = [BitConverter]::ToInt32($b, 0x3C)
  return ([BitConverter]::ToUInt16($b, $pe + 4) -eq 0xAA64)
}

try {
  # ---------------------------------------------------------------- checks
  if (-not (Test-Path $Source))    { Fail "Not found: $Source" }
  if (-not (Test-Path $DriverDir)) { Fail "No $DriverDir - run build-s10.cmd first." }
  $infs = @(Get-ChildItem $DriverDir -Recurse -Filter *.inf)
  foreach ($need in 'Exynos9820Ufs.inf', 'Exynos9820Hsi2c.inf', 'S10Touch.inf') {
    if (-not ($infs | Where-Object Name -eq $need)) { Fail "$need missing under $DriverDir - run build-s10.cmd (Windows cannot boot without the UFS driver)." }
  }
  $UfsVer = $null
  foreach ($i in $infs) {
    $ver = (Select-String -Path $i.FullName -Pattern '^\s*DriverVer\s*=\s*(.+)$' | Select-Object -First 1).Matches.Groups[1].Value
    Write-Host ("       driver {0,-26} {1}" -f $i.Name, $ver)
    if ($i.Name -eq 'Exynos9820Ufs.inf' -and "$ver" -match ',\s*(\d+\.\d+\.\d+\.\d+)') { $UfsVer = [version]$Matches[1] }
    if ($i.Name -match '^Exynos990|^SecTsY79A') { Fail "$($i.Name) is an S20 Ultra (z3s) driver - $DriverDir must hold only the S10 drivers" }
    if ($i.Name -eq 'S6SY761TouchBeyond0.inf' -or $i.Name -eq 'SecTsY761Touch.inf') { Fail "$($i.FullName): an old touch driver binds the same ACPI\SCSY0761 as S10Touch - remove it from $DriverDir" }
  }
  foreach ($opt in @(@('S10eBattery.inf', 'battery / charging'), @('Exynos9820Audio.inf', 'sound'))) {
    if (-not ($infs | Where-Object Name -eq $opt[0])) { Warn "$($opt[0]) not in $DriverDir - no $($opt[1]) in this image" }
  }
  if (-not $NoWifi -and -not ($infs | Where-Object Name -eq 'Exynos9820WlanCx.inf')) {
    Fail "Exynos9820WlanCx.inf not in $DriverDir - run build-s10.cmd (or -NoWifi to build an image without WiFi)"
  }
  # v2.4: the GPU package (out-gpu\, NOT out\: build-s10 keeps it apart) and the power service files
  $GpuInf = $null
  if (-not $NoGpu) {
    $GpuInf = Join-Path $GpuDir 'MaliG76Wddm\MaliG76Wddm.inf'
    if (-not (Test-Path $GpuInf)) { Fail "no $GpuInf - build with build-s10.cmd after 'set S10_GPU=1' (or -NoGpu)" }
    foreach ($f in 'MaliG76Wddm.sys', 'maliumd.dll') {
      if (-not (Test-Path (Join-Path (Split-Path $GpuInf) $f))) { Fail "$f missing in $(Split-Path $GpuInf) - rebuild with S10_GPU=1 (umd\ must hold the UMD)" }
    }
    if (-not (Get-ChildItem (Split-Path $GpuInf) -Filter *.cat)) { Fail "no catalog in $(Split-Path $GpuInf) - the build did not sign it" }
    $gv = (Select-String -Path $GpuInf -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*([\d.]+)' | Select-Object -First 1).Matches.Groups[1].Value
    if ([version]$gv -lt [version]'1.0.72.2') { Warn "MaliG76Wddm $gv - 1.0.72.2+ is the one with the S10 brightness" }
    $uh = (Get-FileHash (Join-Path (Split-Path $GpuInf) 'maliumd.dll') -Algorithm SHA256).Hash.ToLower().Substring(0, 8)
    Ok "GPU: MaliG76Wddm $gv, maliumd.dll $uh (0222-s10.4 = 572f69d7)"
  }
  $PowerDir = Join-Path $DriverDir 'S10ePower'
  if ($NoPower) { $PowerDir = $null }
  elseif (-not (Test-Path (Join-Path $PowerDir 'S10ePower.sys')) -or -not (Test-Path (Join-Path $PowerDir 's10egovernor.exe'))) {
    Warn "no $PowerDir\S10ePower.sys / s10egovernor.exe - clocks stay at the UEFI's boot rates"; $PowerDir = $null
  } else { Ok "power: S10ePower + S10eGovernor$(if ($NoOc) { ', stock clocks' } else { ', GPU 806 / A55 2002 MHz' })" }
  if (-not ($infs | Where-Object Name -eq 'Exynos9820Mfc.inf')) { Warn 'Exynos9820Mfc.inf not in out\ - no hardware video decode (get-wifi-fw.cmd v1.2 + build-s10.cmd v1.14)' }
  $DiagDir = Join-Path $DriverDir 'S10eDiag'
  if (-not (Test-Path (Join-Path $DiagDir 'S10eAgent.exe'))) { Warn "no $DiagDir\S10eAgent.exe - hardware keys (power / Bixby / volume) will not work" ; $DiagDir = $null }
  # v2.3: the WiFi firmware (get-wifi-fw.cmd output) is looked for in every usual place, and required unless -NoWifi
  $WifiFw = $null
  if (-not $NoWifi) {
    # first the folder PowerShell is in right now (where you ran the script from), then next to the script, ...
    $fwDirs = @((Join-Path (Get-Location).Path 'bcm4375'), (Join-Path $PSScriptRoot 'bcm4375'), (Join-Path $DriverDir 'bcm4375'),
                (Join-Path $PSScriptRoot '..\bcm4375'), (Join-Path $OutDir 'bcm4375'), (Join-Path $env:USERPROFILE 'Downloads\bcm4375')) |
              Select-Object -Unique
    $WifiFw = $fwDirs | Where-Object { Test-Path (Join-Path $_ 'bcmdhd_sta.bin_b1') } | Select-Object -First 1
    if (-not $WifiFw) {
      Fail "no bcm4375\bcmdhd_sta.bin_b1 (looked in: $($fwDirs -join ', ')). Phone in TWRP: run get-wifi-fw.cmd first (or -NoWifi)."
    }
    $WifiFw = (Resolve-Path $WifiFw).Path
    $nv = @(Get-ChildItem $WifiFw -Filter 'nvram*' -ErrorAction SilentlyContinue)
    if (-not (Test-Path (Join-Path $WifiFw 'bcmdhd_clm.blob')) -or $nv.Count -eq 0) {
      Fail "$WifiFw is incomplete (needs bcmdhd_sta.bin_b1, bcmdhd_clm.blob and nvram.txt_*) - run get-wifi-fw.cmd again"
    }
    Ok "WiFi firmware: $WifiFw ($((Get-ChildItem $WifiFw -File).Count) files)"
  }
  # The UFS driver's write fence is per board: a driver built for the S10e would write
  # into the S10's Android partitions. build-ufs.ps1 keeps a "UFSBRD01" record in the
  # .sys: board 2 = S10 (beyond1lte), then the fence's first LBA (= the S10's cache).
  $ufsSys = Get-ChildItem $DriverDir -Recurse -Filter 'Exynos9820Ufs.sys' | Select-Object -First 1
  if (-not $ufsSys) { Fail "Exynos9820Ufs.sys missing under $DriverDir" }
  $ub  = [IO.File]::ReadAllBytes($ufsSys.FullName)
  $txt = [Text.Encoding]::ASCII.GetString($ub)
  $at  = $txt.IndexOf('UFSBRD01', [StringComparison]::Ordinal)
  if ($at -lt 0 -or $txt.IndexOf('UFSBRD01', $at + 1, [StringComparison]::Ordinal) -ge 0) {
    Fail "$($ufsSys.FullName) has no (or two) UFSBRD01 board record - build it with build-s10.cmd (build-ufs.ps1 -Board Beyond1), not from the .vcxproj"
  }
  $uBoard = [BitConverter]::ToUInt32($ub, $at + 8); $uFence = [BitConverter]::ToUInt32($ub, $at + 12)
  if ($uBoard -ne 2 -or $uFence -ne $EspLba) {
    Fail "Exynos9820Ufs.sys is built for board $uBoard (fence LBA $uFence), not the Galaxy S10 (board 2, fence $EspLba). Rebuild with build-s10.cmd."
  }
  Ok "UFS driver $UfsVer built for the Galaxy S10 (fence starts at cache LBA $uFence)"

  # UEFI for the flash kit
  $UefiFile = $null; $UefiVer = $null
  if (-not $NoUefi) {
    if ($Uefi) {
      if (-not (Test-Path $Uefi)) { Fail "-Uefi: not found: $Uefi" }
      $UefiFile = Get-Item $Uefi
    } else {
      $dirs = @($PSScriptRoot, $OutDir, (Join-Path $PSScriptRoot '..\out'), (Join-Path $env:USERPROFILE 'Downloads')) | Where-Object { $_ -and (Test-Path $_) }
      $UefiFile = Get-ChildItem $dirs -Filter 'beyond1lte-uefi-v*.img' -File -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match '-v(\d+)\.(\d+)\.(\d+)\.img$' } |
                  Sort-Object { if ($_.Name -match '-v(\d+)\.(\d+)\.(\d+)\.img$') { [version]("{0}.{1}.{2}" -f $Matches[1], $Matches[2], $Matches[3]) } }, LastWriteTime |
                  Select-Object -Last 1
      if (-not $UefiFile) { Fail "no beyond1lte-uefi-v*.img found (looked in: $($dirs -join ', ')). Put the newest UEFI .img next to make-windows.ps1, or use -Uefi <file> / -NoUefi." }
    }
    $hb = New-Object byte[] 8
    $fs = [IO.File]::OpenRead($UefiFile.FullName); try { [void]$fs.Read($hb, 0, 8) } finally { $fs.Close() }
    if ([Text.Encoding]::ASCII.GetString($hb) -ne 'ANDROID!') { Fail "$($UefiFile.Name) is not a boot image (no ANDROID! header)" }
    if ($UefiFile.Length -gt 57671680) { Fail "$($UefiFile.Name) is larger than the S10 boot partition (14080 x 4096 B)" }
    if ($UefiFile.Name -match '-v(\d+)\.(\d+)\.(\d+)\.img$') { $UefiVer = [version]("{0}.{1}.{2}" -f $Matches[1], $Matches[2], $Matches[3]) }
    if ($UefiFile.Name -notmatch '^beyond1lte-') { Warn "$($UefiFile.Name) is not named beyond1lte-uefi-v*.img - make sure it is the Galaxy S10 UEFI" }
    Ok "UEFI for the kit: $($UefiFile.FullName)"
  }
  $cs = Join-Path $PSScriptRoot 'z3s-disk.cs'
  if (-not (Test-Path $cs)) { Fail 'z3s-disk.cs missing next to make-windows.ps1' }
  foreach ($f in 'set-esp-type.ps1') {
    if (-not (Test-Path (Join-Path $PSScriptRoot $f))) { Fail "$f missing next to make-windows.ps1" }
  }
  # C# types cannot be reloaded in a PowerShell window: if an older z3s-disk.cs is
  # already loaded (an earlier run in this window), the new types are missing.
  if (-not ('Z3s.Gpt' -as [type])) { Add-Type -TypeDefinition (Get-Content -Raw $cs) }
  foreach ($t in 'Z3s.Pieces', 'Z3s.FlashCmd', 'Z3s.Seg', 'Z3s.GptType', 'Z3s.GptRepair') {
    if (-not ($t -as [type])) { Fail "an older z3s-disk.cs is loaded in this PowerShell window ($t missing). Close this window, open a NEW PowerShell (Admin) and run again (add -Resume to reuse the image already built)." }
  }

  $free = (Get-PSDrive $OutDir.Substring(0, 1)).Free
  if ($free -lt 40GB) { Fail ("{0:N0} GB free on {1}: - need about 40 GB (or use -OutDir on another drive)" -f ($free / 1GB), $OutDir.Substring(0, 2)) }

  if (Test-Path $Vhd) { Dismount-DiskImage -ImagePath $Vhd -ErrorAction SilentlyContinue | Out-Null }
  if ($Resume) {
    if (-not (Test-Path $Vhd)) { Fail "-Resume: no $Vhd from an earlier run" }
    if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }
  } else {
    foreach ($d in $Work, $Dest) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
  }
  New-Item -ItemType Directory -Force $Work, $Dest | Out-Null

  # --------------------------------------------------------------- 1. source
  Step '1. Windows image'
  $SourceFull = (Resolve-Path $Source).Path
  if ($SourceFull -match '\.iso$') {
    $IsoPath = $SourceFull
    $iso = Mount-DiskImage -ImagePath $IsoPath -PassThru
    $isoL = ($iso | Get-Volume).DriveLetter
    $Wim = @("${isoL}:\sources\install.wim", "${isoL}:\sources\install.esd") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Wim) { Fail "no sources\install.wim or install.esd in $IsoPath" }
  } elseif (Test-Path $SourceFull -PathType Container) {
    # an extracted ISO / uupdump output folder
    $Wim = @('sources\install.wim', 'sources\install.esd', 'install.wim', 'install.esd') |
           ForEach-Object { Join-Path $SourceFull $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Wim) {
      if (Get-ChildItem $SourceFull -Recurse -Include *.cab, *.esd -ErrorAction SilentlyContinue | Select-Object -First 1) {
        Fail "$SourceFull holds raw UUP files (.esd/.cab), not an install image. Run uupdump's convert-UUP.cmd (or uup_download_windows.cmd again) to get the ISO, then -Source <iso>."
      }
      Fail "no install.wim / install.esd in $SourceFull"
    }
  } else {
    $Wim = $SourceFull
  }
  if ($Wim -match '\.esd$') {
    Write-Host '       install.esd (LZMS): works, applying is just slower than a .wim'
  }
  $images = @(Get-WindowsImage -ImagePath $Wim)
  # A self-contained ESD (install.esd, or the Media Creation Tool kind with
  # "Windows Setup Media" + PE + Setup + editions) is GBs. A UUP edition ESD
  # is only metadata (tens of MB) pointing into the other UUP .esd files.
  if ($Wim -match '\.esd$' -and (Get-Item $Wim).Length -lt 1GB) {
    Fail ("$(Split-Path $Wim -Leaf) is only {0:N0} MB: a raw UUP edition file that needs the other .esd files. Run uupdump's convert-UUP.cmd to make the ISO, then -Source <iso>." -f ((Get-Item $Wim).Length / 1MB))
  }
  foreach ($im in $images) { Write-Host ("       {0,2}  {1}" -f $im.ImageIndex, $im.ImageName) }
  if ($Index -eq 0) {
    $pick = $images | Where-Object { $_.ImageName -eq "Windows 11 $Edition" } | Select-Object -First 1
    if (-not $pick) { $pick = $images | Where-Object { $_.ImageName -like "* $Edition" } | Select-Object -First 1 }
    if (-not $pick) { Fail "no edition '$Edition' - pick one with -Index <number> from the list above" }
    $Index = $pick.ImageIndex
  }
  $info = Get-WindowsImage -ImagePath $Wim -Index $Index
  $arch = "$($info.Architecture)"
  if (($arch -ne '12') -and ($arch -notmatch 'arm64')) { Fail "image $Index is '$arch', not ARM64" }
  $Lang = 'en-US'
  try { $Lang = (("" + $info.Languages[$info.DefaultLanguageIndex]) -split ' ')[0] } catch { }
  if ($Lang -notmatch '^[a-z]{2,3}-[A-Za-z]{2,4}') { $Lang = 'en-US' }
  Ok ("image {0}: {1}, {2}, build {3}, {4}" -f $Index, $info.ImageName, $arch, $info.Version, $Lang)

  # ------------------------------------------------------------ 2. the VHDX
  if (-not $Resume) {
  Step '2. 4K-sector VHDX shaped like the phone'
  [uint64]$vhdSize = 1MB + $EspBytes + $WinBytes + 16MB
  $made = $false
  if (Get-Command New-VHD -ErrorAction SilentlyContinue) {
    try {
      New-VHD -Path $Vhd -SizeBytes $vhdSize -Dynamic -LogicalSectorSizeBytes 4096 -PhysicalSectorSizeBytes 4096 | Out-Null
      $made = $true
    } catch { Warn "New-VHD failed ($($_.Exception.Message)) - using virtdisk.dll" }
  }
  if (-not $made) { [Z3s.Vhd]::Create4Kn($Vhd, $vhdSize) }
  Mount-DiskImage -ImagePath $Vhd -NoDriveLetter | Out-Null
  $disk = Get-VhdDisk
  if ($disk.IsOffline) { Set-Disk -Number $disk.Number -IsOffline $false; $disk = Get-VhdDisk }
  if ($disk.LogicalSectorSize -ne 4096) { Fail "VHDX has $($disk.LogicalSectorSize)-byte sectors, need 4096" }
  Initialize-Disk -Number $disk.Number -PartitionStyle GPT
  Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | Remove-Partition -Confirm:$false
  $p1 = New-Partition -DiskNumber $disk.Number -Offset $EspOff -Size $EspBytes -GptType $BasicData
  $p2 = New-Partition -DiskNumber $disk.Number -Offset $WinOff -Size $WinBytes -GptType $BasicData
  if ($p1.Size -ne $EspBytes -or $p2.Size -ne $WinBytes) { Fail "partition sizes $($p1.Size) / $($p2.Size)" }

  # phone GUIDs, written straight into both GPT copies while the disk is offline
  Set-Disk -Number $disk.Number -IsOffline $true
  $rd = New-Object Z3s.RawDisk ("\\.\PhysicalDrive$($disk.Number)", $true)
  try {
    Write-Host ([Z3s.Gpt]::SetGuids($rd, $SS, $DiskGuid, [long[]]@($EspOff, $WinOff), [guid[]]@($EspGuid, $WinGuid)))
  } finally { $rd.Dispose() }
  Dismount-DiskImage -ImagePath $Vhd | Out-Null
  Mount-DiskImage -ImagePath $Vhd -NoDriveLetter | Out-Null
  $disk = Get-VhdDisk
  if ($disk.IsOffline)  { Set-Disk -Number $disk.Number -IsOffline $false }
  if ($disk.IsReadOnly) { Set-Disk -Number $disk.Number -IsReadOnly $false }
  $disk = Get-VhdDisk
  $parts = @(Get-Partition -DiskNumber $disk.Number)
  $pEsp = $parts | Where-Object Offset -eq $EspOff
  $pWin = $parts | Where-Object Offset -eq $WinOff
  if ($disk.Guid -ne $DiskGuid.ToString('B'))  { Fail "disk GUID is $($disk.Guid)" }
  if ($pEsp.Guid -ne $EspGuid.ToString('B'))   { Fail "ESP GUID is $($pEsp.Guid)" }
  if ($pWin.Guid -ne $WinGuid.ToString('B'))   { Fail "Windows GUID is $($pWin.Guid)" }
  Ok 'disk + partition GUIDs = phone (cache / userdata)'

  Format-Volume -Partition $pEsp -FileSystem FAT32 -AllocationUnitSize 4096 -NewFileSystemLabel 'ESP' -Confirm:$false -Force | Out-Null
  Format-Volume -Partition $pWin -FileSystem NTFS  -AllocationUnitSize 4096 -NewFileSystemLabel 'Windows' -Confirm:$false -Force | Out-Null
  $S = Letter $pEsp
  $W = Letter $pWin
  Ok ("ESP ${S}: (FAT32 600 MiB), Windows ${W}: (NTFS {0} MiB of the {1:N0} B userdata), 4K sectors" -f ($WinBytes / 1MB), $WinPartBytes)

  # ------------------------------------------------------------- 3. apply
  Step "3. dism /Apply-Image (10-20 min)"
  $applyArgs = @('/Apply-Image', "/ImageFile:$Wim", "/Index:$Index", "/ApplyDir:${W}:\")
  if ($Compact) { $applyArgs += '/Compact' }
  & dism.exe @applyArgs
  if ($LASTEXITCODE) { Fail "dism /Apply-Image ($LASTEXITCODE)" }
  if ($IsoPath) { Dismount-DiskImage -ImagePath $IsoPath | Out-Null; $IsoPath = $null }
  Ok 'image applied'

  # -------------------------------------------------------- 4. lite + drivers
  if (-not $NoLite) {
    Step '4a. removing inbox apps'
    $junk = 'Clipchamp.Clipchamp', 'Microsoft.549981C3F5F10', 'Microsoft.BingNews', 'Microsoft.BingWeather',
            'Microsoft.GamingApp', 'Microsoft.GetHelp', 'Microsoft.Getstarted', 'Microsoft.MicrosoftOfficeHub',
            'Microsoft.MicrosoftSolitaireCollection', 'Microsoft.People', 'Microsoft.PowerAutomateDesktop',
            'Microsoft.Todos', 'Microsoft.WindowsCommunicationsApps', 'Microsoft.WindowsFeedbackHub',
            'Microsoft.WindowsMaps', 'Microsoft.Xbox.TCUI', 'Microsoft.XboxGameOverlay', 'Microsoft.XboxGamingOverlay',
            'Microsoft.XboxIdentityProvider', 'Microsoft.XboxSpeechToTextOverlay', 'Microsoft.YourPhone',
            'Microsoft.ZuneMusic', 'Microsoft.ZuneVideo', 'MicrosoftCorporationII.MicrosoftFamily',
            'MicrosoftCorporationII.QuickAssist', 'MicrosoftTeams', 'MicrosoftWindows.Client.WebExperience'
    foreach ($a in @(Get-AppxProvisionedPackage -Path "${W}:\")) {
      if ($junk -contains $a.DisplayName) {
        try { Remove-AppxProvisionedPackage -Path "${W}:\" -PackageName $a.PackageName | Out-Null; Write-Host "       - $($a.DisplayName)" }
        catch { Warn "could not remove $($a.DisplayName)" }
      }
    }
  }

  Step '4b. drivers'
  & dism.exe "/Image:${W}:\" /Add-Driver "/Driver:$DriverDir" /Recurse /ForceUnsigned
  if ($LASTEXITCODE) { Fail "dism /Add-Driver ($LASTEXITCODE)" }
  # v2.3: every package of out\ must be in the image, by INF name
  $inImage = @(Get-WindowsDriver -Path "${W}:\" | ForEach-Object { Split-Path $_.OriginalFileName -Leaf })
  $missing = @()
  foreach ($i in $infs) {
    if ($inImage -contains $i.Name) { Write-Host ("       + {0}" -f $i.Name) } else { $missing += $i.Name }
  }
  if ($missing.Count) { Fail "not in the image after Add-Driver: $($missing -join ', ')" }
  Ok "all $($infs.Count) driver packages of $DriverDir are in the image"
  if ($GpuInf) {
    & dism.exe "/Image:${W}:\" /Add-Driver "/Driver:$GpuInf" /ForceUnsigned
    if ($LASTEXITCODE) { Fail "dism /Add-Driver MaliG76Wddm ($LASTEXITCODE)" }
    if (-not (Get-WindowsDriver -Path "${W}:\" | Where-Object { (Split-Path $_.OriginalFileName -Leaf) -eq 'MaliG76Wddm.inf' })) { Fail 'MaliG76Wddm.inf not in the image after Add-Driver' }
    Ok 'MaliG76Wddm (GPU + its user-mode driver) is in the image'
  }

  # S10 kit v1.1: WiFi blobs where Exynos9820WlanCx looks for them, and the key agent installed
  # by SetupComplete.cmd at the end of Setup (as the S10e's s10e-setupcomplete.cmd does).
  if ($WifiFw) {
    $fwDst = "${W}:\Windows\System32\drivers\bcm4375"
    New-Item -ItemType Directory -Force $fwDst | Out-Null
    Copy-Item (Join-Path $WifiFw '*') $fwDst -Force
    Ok "WiFi firmware -> \Windows\System32\drivers\bcm4375 ($((Get-ChildItem $fwDst).Count) files)"
  }
  $prov = "${W}:\S10Prov"
  New-Item -ItemType Directory -Force $prov | Out-Null
  # v2.3: the whole kit inside Windows too (C:\S10Prov\kit): out\, bcm4375\, setup\ and the in-Windows scripts,
  # so install-in-windows.ps1 / collect-logs.ps1 are there without a USB stick or a push from TWRP.
  $kit = "$prov\kit"
  New-Item -ItemType Directory -Force $kit | Out-Null
  Copy-Item $DriverDir "$kit\out" -Recurse -Force
  if ($WifiFw) { Copy-Item $WifiFw "$kit\bcm4375" -Recurse -Force }
  Copy-Item (Join-Path $PSScriptRoot 'setup') "$kit\setup" -Recurse -Force
  foreach ($f in 'install-in-windows.ps1', 'collect-logs.ps1', 'auto-install.ps1', 'kit-watch.ps1',
               'gpu-s10.ps1', 'power-s10.ps1', 'panel-s10.ps1', 'video-s10.ps1', 'gpu-validate.ps1', 'drag-probe.ps1',
               'S10eSpeakerTrim.cs') {   # v2.4.1: SetupComplete v1.3 compiles the speaker app from it
    if (Test-Path (Join-Path $PSScriptRoot $f)) { Copy-Item (Join-Path $PSScriptRoot $f) $kit -Force }
  }
  # v2.4: the GPU package and the test tools / media next to the scripts that use them
  if ($GpuInf) { Copy-Item $GpuDir "$kit\out-gpu" -Recurse -Force }
  foreach ($d in 'tools', 'video') { if (Test-Path (Join-Path $PSScriptRoot $d)) { Copy-Item (Join-Path $PSScriptRoot $d) "$kit\$d" -Recurse -Force } }
  # v2.4: what SetupComplete.cmd finishes on the phone (flags) + the power service files where power-s10 keeps them
  if ($GpuInf) { '' | Set-Content "$prov\final-gpu.flg" }
  if ($PowerDir) {
    New-Item -ItemType Directory -Force "$prov\power" | Out-Null
    Copy-Item (Join-Path $PowerDir '*') "$prov\power" -Force
    '' | Set-Content "$prov\final-power.flg"
    if (-not $NoOc) { '' | Set-Content "$prov\final-oc.flg" }
  }
  # one-click log collector on everyone's desktop (C:\log.zip -> TWRP: adb pull /data/log.zip)
  if (Test-Path (Join-Path $PSScriptRoot 's10-log.cmd')) {
    Copy-Item (Join-Path $PSScriptRoot 's10-log.cmd') "${W}:\Users\Public\Desktop\s10-log.cmd" -Force
  }
  '' | Set-Content "$kit\install.done"   # everything is already installed: the logon launcher has nothing to do
  Ok "kit -> C:\S10Prov\kit (out\, bcm4375\, setup\, install-in-windows / collect-logs)"
  if ($DiagDir) {
    New-Item -ItemType Directory -Force "$prov\S10eDiag" | Out-Null
    Copy-Item (Join-Path $DiagDir '*') "$prov\S10eDiag" -Force
  }
  $scr = "${W}:\Windows\Setup\Scripts"
  New-Item -ItemType Directory -Force $scr | Out-Null
  Copy-Item (Join-Path $PSScriptRoot 'setup\SetupComplete.cmd') (Join-Path $scr 'SetupComplete.cmd') -Force
  Ok "SetupComplete.cmd: key agent$(if (-not $DiagDir) { ' (missing - keys off)' }) + power settings at the end of Setup"

  # ------------------------------------------------------------ 5. registry
  Step '5. registry'
  Load-Hive 'z3s_SYSTEM'   "${W}:\Windows\System32\config\SYSTEM"
  Load-Hive 'z3s_SOFTWARE' "${W}:\Windows\System32\config\SOFTWARE"
  Load-Hive 'z3s_DEFAULT'  "${W}:\Users\Default\NTUSER.DAT"
  $cur = 1
  $q = cmd /c "reg query HKLM\z3s_SYSTEM\Select /v Current 2>nul"
  if (-not $q) { $q = & reg.exe query 'HKLM\z3s_SYSTEM\Select' /v Current }
  if ("$q" -match 'Current\s+REG_DWORD\s+0x([0-9a-fA-F]+)') { $cur = [Convert]::ToInt32($Matches[1], 16) }
  $CS = 'z3s_SYSTEM\ControlSet{0:D3}' -f $cur

  RegSet "$CS\Services\mountmgr" 'NoAutoMount' REG_DWORD 1                   # no letters for 30 Android partitions
  if ($GpuInf) {   # v2.4: = gpu-s10.ps1 Set-TdrDelay $true (part of "final")
    RegSet "$CS\Control\GraphicsDrivers" 'TdrDelay' REG_DWORD 10
    RegSet "$CS\Control\GraphicsDrivers" 'TdrDdiDelay' REG_DWORD 20
    RegSet "$CS\Control\GraphicsDrivers\Scheduler" 'EnablePreemption' REG_DWORD 0
  }
  RegSet "$CS\Control\Session Manager\Power" 'HiberbootEnabled' REG_DWORD 0  # no fast startup: every boot is cold
  RegSet "$CS\Control\Power" 'HibernateEnabled' REG_DWORD 0
  RegSet "$CS\Control\Power" 'HibernateEnabledDefault' REG_DWORD 0
  RegSet "$CS\Control\DeviceGuard" 'EnableVirtualizationBasedSecurity' REG_DWORD 0
  RegSet "$CS\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" 'Enabled' REG_DWORD 0
  RegSet "$CS\Control\CI\Policy" 'VerifiedAndReputablePolicyState' REG_DWORD 0   # Smart App Control off
  # TPM 2.0 / Secure Boot / RAM / CPU: Setup and feature upgrades skip the checks
  foreach ($k in 'BypassTPMCheck', 'BypassSecureBootCheck', 'BypassRAMCheck', 'BypassCPUCheck', 'BypassStorageCheck') {
    RegSet 'z3s_SYSTEM\Setup\LabConfig' $k REG_DWORD 1
  }
  RegSet 'z3s_SYSTEM\Setup\MoSetup' 'AllowUpgradesWithUnsupportedTPMOrCPU' REG_DWORD 1
  RegSet 'z3s_DEFAULT\Software\Microsoft\PCHC' 'UpgradeEligibility' REG_DWORD 1
  RegSet 'z3s_SOFTWARE\Microsoft\Windows\CurrentVersion\OOBE' 'BypassNRO' REG_DWORD 1
  if (-not $NoLite) {
    foreach ($svc in 'SysMain', 'WSearch', 'DiagTrack') { RegSet "$CS\Services\$svc" 'Start' REG_DWORD 4 }
    RegSet 'z3s_SOFTWARE\Microsoft\Windows\CurrentVersion\ReserveManager' 'ShippedWithReserves' REG_DWORD 0
    RegSet 'z3s_SOFTWARE\Policies\Microsoft\Windows\Windows Chat' 'ChatIcon' REG_DWORD 3
    RegSet 'z3s_SOFTWARE\Policies\Microsoft\Dsh' 'AllowNewsAndInterests' REG_DWORD 0
  }
  # new users: scaling and fewer effects (everything is drawn by the CPU)
  RegSet 'z3s_DEFAULT\Control Panel\Desktop' 'LogPixels' REG_DWORD ([int](96 * $Scale / 100))
  RegSet 'z3s_DEFAULT\Control Panel\Desktop' 'Win8DpiScaling' REG_DWORD 1
  RegSet 'z3s_DEFAULT\Control Panel\Desktop\WindowMetrics' 'MinAnimate' REG_SZ '0'
  RegSet 'z3s_DEFAULT\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects' 'VisualFXSetting' REG_DWORD 2
  RegSet 'z3s_DEFAULT\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' 'TaskbarAnimations' REG_DWORD 0
  RegSet 'z3s_DEFAULT\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' 'EnableTransparency' REG_DWORD 0
  Unload-Hives
  Ok "test-signed drivers allowed, no fast startup/VBS, TPM + Secure Boot checks off, scale $Scale %"

  # ------------------------------------------------------------ 6. unattend
  New-Item -ItemType Directory -Force "${W}:\Windows\Panther" | Out-Null
  $arch64 = 'processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS"'
  @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup" $arch64>
      <ComputerName>GALAXY-S10</ComputerName>
      <TimeZone>SE Asia Standard Time</TimeZone>
    </component>
  </settings>
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-International-Core" $arch64>
      <InputLocale>0409:00000409</InputLocale>
      <SystemLocale>$Lang</SystemLocale>
      <UILanguage>$Lang</UILanguage>
      <UserLocale>vi-VN</UserLocale>
    </component>
    <component name="Microsoft-Windows-Shell-Setup" $arch64>
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
      <UserAccounts>
        <LocalAccounts>
          <LocalAccount wcm:action="add">
            <Name>$UserName</Name>
            <DisplayName>$UserName</DisplayName>
            <Group>Administrators</Group>
            <Password><Value></Value><PlainText>true</PlainText></Password>
          </LocalAccount>
        </LocalAccounts>
      </UserAccounts>
      <AutoLogon>
        <Enabled>true</Enabled>
        <Username>$UserName</Username>
        <Password><Value></Value><PlainText>true</PlainText></Password>
        <LogonCount>1</LogonCount>
      </AutoLogon>
    </component>
  </settings>
</unattend>
"@ | Set-Content "${W}:\Windows\Panther\unattend.xml" -Encoding UTF8
  Ok "unattend.xml: local admin '$UserName' (no password), no Microsoft account / network page"

  } else {
  Step '2-6. -Resume: reusing work\windows.vhdx'
  if ($IsoPath) { Dismount-DiskImage -ImagePath $IsoPath | Out-Null; $IsoPath = $null }
  Mount-DiskImage -ImagePath $Vhd -NoDriveLetter | Out-Null
  $disk = Get-VhdDisk
  if ($disk.IsOffline)  { Set-Disk -Number $disk.Number -IsOffline $false }
  if ($disk.IsReadOnly) { Set-Disk -Number $disk.Number -IsReadOnly $false }
  $disk = Get-VhdDisk
  $parts = @(Get-Partition -DiskNumber $disk.Number)
  $pEsp = $parts | Where-Object Offset -eq $EspOff
  $pWin = $parts | Where-Object Offset -eq $WinOff
  if (-not $pEsp -or -not $pWin -or $pEsp.Guid -ne $EspGuid.ToString('B') -or $pWin.Guid -ne $WinGuid.ToString('B')) {
    Fail 'work\windows.vhdx is not laid out like the phone - run without -Resume'
  }
  $S = Letter $pEsp
  $W = Letter $pWin
  if (-not (Test-Path "${W}:\Windows\Panther\unattend.xml")) { Fail 'work\windows.vhdx did not get past step 6 - run without -Resume' }
  Ok "ESP ${S}:, Windows ${W}: (image, drivers, registry, unattend already done)"
  }

  # ---------------------------------------------------------- 7. boot files
  Step '7. boot files + BCD'
  $mb = "${S}:\EFI\Microsoft\Boot"
  & robocopy.exe "${W}:\Windows\Boot\EFI" $mb /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { Fail "copy boot files ($LASTEXITCODE)" }
  & robocopy.exe "${W}:\Windows\Boot\Fonts" "$mb\Fonts" /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { Fail "copy boot fonts ($LASTEXITCODE)" }
  New-Item -ItemType Directory -Force "${S}:\EFI\Boot" | Out-Null
  Copy-Item "$mb\bootmgfw.efi" "${S}:\EFI\Boot\BOOTAA64.EFI" -Force
  if (-not (Is-Arm64Pe "${S}:\EFI\Boot\BOOTAA64.EFI")) { Fail 'bootmgfw.efi in the image is not ARM64' }

  $B = "$mb\BCD"
  if (Test-Path $B) { Remove-Item $B -Force }
  & bcdedit.exe /createstore $B | Out-Null
  if ($LASTEXITCODE) { Fail 'bcdedit /createstore' }
  function Bcd { $ErrorActionPreference = 'Continue'; $o = & bcdedit.exe /store $B @args 2>&1; if ($LASTEXITCODE) { Fail "bcdedit $args : $o" }; return "$o" }
  Bcd /create '{bootmgr}' /d 'Windows Boot Manager' | Out-Null
  Bcd /set '{bootmgr}' path '\EFI\Microsoft\Boot\bootmgfw.efi' | Out-Null
  Bcd /set '{bootmgr}' locale $Lang | Out-Null
  Bcd /set '{bootmgr}' timeout 0 | Out-Null
  $id = [regex]::Match((Bcd /create /d 'Windows 11 (Galaxy S10)' /application osloader), '\{[0-9a-fA-F-]{36}\}').Value
  if (-not $id) { Fail 'bcdedit /create osloader: no id' }
  foreach ($kv in @(
      @('path', '\Windows\system32\winload.efi'), @('systemroot', '\Windows'), @('locale', $Lang),
      @('testsigning', 'on'), @('nointegritychecks', 'on'), @('recoveryenabled', 'No'),
      @('bootstatuspolicy', 'IgnoreAllFailures'), @('hypervisorlaunchtype', 'Off'), @('nx', 'OptIn'))) {
    Bcd /set $id $kv[0] $kv[1] | Out-Null
  }
  Bcd /set '{bootmgr}' default $id | Out-Null
  Bcd /set '{bootmgr}' displayorder $id | Out-Null
  # Devices: bcdedit turns partition=X: on an attached VHDX into vhd=[..]\windows.vhdx,
  # so set them through the BCD WMI provider as GPT partitions qualified by the
  # phone's disk + partition GUIDs (same as the S9+ installer). Separate process,
  # so the provider lets go of the store before the VHDX is detached.
  $wmi = Join-Path $env:TEMP 'z3s-bcd-devices.ps1'
  $dg = $DiskGuid.ToString('B'); $eg = $EspGuid.ToString('B'); $wg = $WinGuid.ToString('B')
  @"
`$ErrorActionPreference = 'Stop'
function Call(`$o, [string]`$m, [hashtable]`$a) {
  `$r = Invoke-CimMethod -InputObject `$o -MethodName `$m -Arguments `$a
  if (-not `$r.ReturnValue) { throw "BCD rejected `$m" }
  `$r
}
`$open = Invoke-CimMethod -Namespace root\WMI -ClassName BcdStore -MethodName OpenStore -Arguments @{ File = '$B' }
if (-not `$open.ReturnValue) { throw 'cannot open the BCD store' }
`$store = `$open.Store
`$mgr = (Call `$store 'OpenObject' @{ Id = '{9dea862c-5cdd-4e70-acc1-f32b344d4795}' }).Object
`$ldr = (Call `$store 'OpenObject' @{ Id = '$id' }).Object
`$null = Call `$mgr 'SetQualifiedPartitionDeviceElement' @{ Type = [uint32]0x11000001; PartitionStyle = [uint32]1; DiskSignature = '$dg'; PartitionIdentifier = '$eg' }
foreach (`$t in [uint32]0x11000001, [uint32]0x21000001) {
  `$null = Call `$ldr 'SetQualifiedPartitionDeviceElement' @{ Type = `$t; PartitionStyle = [uint32]1; DiskSignature = '$dg'; PartitionIdentifier = '$wg' }
}
foreach (`$x in @(@(`$mgr, 0x11000001), @(`$ldr, 0x11000001), @(`$ldr, 0x21000001))) {
  `$d = (Call `$x[0] 'GetElement' @{ Type = [uint32]`$x[1] }).Element.Device
  'readback {0:x8} style={1} disk={2} part={3}' -f `$x[1], `$d.PartitionStyle, `$d.DiskSignature, `$d.PartitionIdentifier
}
'Z3S_BCD_DEVICES_SET'
"@ | Set-Content $wmi -Encoding ASCII
  $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $out = & { $ErrorActionPreference = 'Continue'; & $ps -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $wmi 2>&1 | ForEach-Object { "$_" } }
  Remove-Item $wmi -Force -ErrorAction SilentlyContinue
  $out | ForEach-Object { Write-Host "       $_" }
  if (-not ($out -match 'Z3S_BCD_DEVICES_SET')) { Fail 'setting the phone boot devices through WMI failed (output above)' }
  foreach ($r in @($out | Where-Object { $_ -match '^readback' })) {
    if ($r -match 'part=\{' -and ($r -notmatch [regex]::Escape($dg) -or ($r -notmatch [regex]::Escape($eg) -and $r -notmatch [regex]::Escape($wg)))) {
      Fail "BCD device read back wrong: $r"
    }
  }
  Start-Sleep 3
  # bcdedit /createstore makes a NON-system store; Setup's specialize (spbcd) then stops with
  # "BCD: File is not system store" -> "could not configure Windows to run on this computer's
  # hardware" (seen on the phone with v1.5). bcdboot marks its store like this:
  $loaded = $false
  for ($t = 0; $t -lt 10 -and -not $loaded; $t++) {
    $null = & { $ErrorActionPreference = 'Continue'; & reg.exe load HKLM\z3s_BCD $B 2>&1 }
    if ($LASTEXITCODE) { Start-Sleep 2 } else { $loaded = $true; $Hives.Add('HKLM\z3s_BCD') }
  }
  if (-not $loaded) { Fail "reg load $B" }
  RegSet 'z3s_BCD\Description' 'System' REG_DWORD 1
  RegSet 'z3s_BCD\Description' 'TreatAsSystem' REG_DWORD 1
  Unload-Hives
  Ok 'BCD marked as the system store (Description\System, TreatAsSystem)'
  $enum = & { $ErrorActionPreference = 'Continue'; & bcdedit.exe /store $B /enum all /v 2>&1 | ForEach-Object { "$_" } }
  $enum | Set-Content (Join-Path $Dest 'bcd.txt')
  Ok "BCD: bootmgr on cache, Windows on userdata (GUID based), test signing on"

  # ------------------------------------------------------------- 8. export
  Step '8. export'
  Write-VolumeCache -DriveLetter $W -ErrorAction SilentlyContinue
  Write-VolumeCache -DriveLetter $S -ErrorAction SilentlyContinue
  Dismount-DiskImage -ImagePath $Vhd | Out-Null
  Mount-DiskImage -ImagePath $Vhd -Access ReadOnly -NoDriveLetter | Out-Null
  $disk = Get-VhdDisk
  $vol = $null
  for ($t = 0; $t -lt 30 -and -not $vol; $t++) {
    $pWin = Get-Partition -DiskNumber $disk.Number | Where-Object Offset -eq $WinOff
    $vol  = @($pWin.AccessPaths | Where-Object { $_ -like '\\?\Volume*' })[0]
    if (-not $vol) { Start-Sleep 1 }
  }
  if (-not $vol) { $vol = (Get-Volume -Partition $pWin).Path }
  if (-not $vol) { Fail 'NTFS volume did not come back after the read-only remount' }
  $vol = $vol.TrimEnd('\')
  $clusters = [long]0
  $bmp = [Z3s.Vol]::Bitmap($vol, [long]($WinBytes / $SS), [ref]$clusters)
  $ext = [Z3s.Export]::Plan($bmp, $clusters, $SS, $WinBytes, 8)
  $mib = 0; foreach ($x in $ext) { $mib += $x.CountMiB }
  Ok ("NTFS: {0:N0} clusters, {1} pieces, {2:N0} MiB to write" -f $clusters, $ext.Count, $mib)

  $say = [Action[string]] { param($m) Write-Host "       $m" }
  $segs = New-Object 'System.Collections.Generic.List[Z3s.Seg]'
  $rd = New-Object Z3s.RawDisk ("\\.\PhysicalDrive$($disk.Number)", $false)
  try {
    $boot = New-Object byte[] $SS
    $rd.Read($EspOff, $boot, $SS)
    $bk = [BitConverter]::ToUInt16($boot, 0x32)
    $g = New-Object Z3s.Seg
    $g.Target = 0; $g.PartOffset = $EspOff; $g.StartMiB = 0; $g.CountMiB = $EspBytes / 1MB
    $g.BootSectors = [long[]]@(0, ([long]$bk * $SS)); $g.HiddenLba = [uint32]$EspLba
    $segs.Add($g)
    foreach ($x in $ext) {
      $g = New-Object Z3s.Seg
      $g.Target = 1; $g.PartOffset = $WinOff; $g.StartMiB = $x.StartMiB; $g.CountMiB = $x.CountMiB
      $g.BootSectors = [long[]]@(0, ($WinBytes - $SS)); $g.HiddenLba = [uint32]$WinLba
      $segs.Add($g)
    }
    Write-Host "       writing the flash files ($($mib + $EspBytes / 1MB) MiB in pieces of <= 256 MiB)..."
    $pieces = [Z3s.Pieces]::Write($rd, $SS, $segs, 256, $Dest, $say)
  } finally { $rd.Dispose() }
  Dismount-DiskImage -ImagePath $Vhd | Out-Null
  if (-not $KeepVhd) { Remove-Item $Work -Recurse -Force }

  # ------------------------------------------------------- 9. flash kit
  # v2.1: files straight from the VHDX + flash-windows.cmd with plain adb/dd per file
  # (one 11 GB adb stream died twice at 8949 MiB; this way wrote 12 GB fine).
  foreach ($f in 'set-esp-type.ps1', 'z3s-disk.cs') { Copy-Item (Join-Path $PSScriptRoot $f) $Dest -Force }
  $uefiDst = $null
  if ($UefiFile) {
    $uefiDst = Join-Path $Dest 'uefi.img'
    Copy-Item $UefiFile.FullName $uefiDst -Force
    Set-Content (Join-Path $Dest 'uefi.txt') $UefiFile.Name -Encoding ASCII
  }
  $what = "$($info.ImageName) build $($info.Version)  $Lang  user=$UserName  lite=$(-not $NoLite)  ufs=$UfsVer(beyond1)  uefi=$(if ($UefiFile) { $UefiFile.Name } else { 'none' })"
  # cacheSize / userdataSize here are what flash-windows.cmd compares with blockdev on the
  # phone, so the WHOLE userdata partition goes in, not the (MiB-rounded) NTFS volume.
  [Z3s.FlashCmd]::Write($Dest, $pieces, $EspBytes, $WinPartBytes, $uefiDst, $(if ($UefiFile) { $UefiFile.Name } else { '' }), $what, $ScriptVersion)
  # S10 guard: refuse any phone that is not a Galaxy S10, before the size check and before
  # set-esp-type touches the GPT (the S20 Ultra's layout differs anyway, but say it plainly).
  $fc = Join-Path $Dest 'flash-windows.cmd'
  $lines = [System.Collections.Generic.List[string]](Get-Content $fc)
  $gi = $lines.FindIndex([Predicate[string]]{ param($l) $l -like 'adb get-state*' })
  if ($gi -lt 0) { Fail 'flash-windows.cmd: adb get-state line not found - z3s-disk.cs changed?' }
  $lines.InsertRange($gi + 1, [string[]]@(
    'set "BL="',
    'for /f %%B in (''adb shell "getprop ro.boot.bootloader"'') do set "BL=%%B"',
    'echo %BL%| findstr /b /c:"G973" >nul || (echo [FAIL] this phone''s bootloader is "%BL%" - this kit is for the Galaxy S10 ^(G973*^) only, nothing written & goto :end)',
    'echo [ OK ] Galaxy S10, bootloader %BL%'))
  Set-Content $fc $lines -Encoding ASCII
  "$ScriptVersion  $what" | Set-Content (Join-Path $Dest 'manifest.txt') -Encoding ASCII

  Write-Host ''
  Ok ("pieces : {0}\pieces  ({1} files, list: pieces.txt)" -f $Dest, $pieces.Count)
  if ($UefiFile) { Ok "uefi   : $($UefiFile.Name) (flashed to boot by the same flash-windows.cmd)" }
  Ok "flash  : $Dest\flash-windows.cmd  (phone in TWRP, double-click: ESP type + Windows + UEFI + checks + reboot)"
  Ok "log    : $(Join-Path $OutDir 'make-windows.log')"
}
catch {
  Write-Host "$_" -ForegroundColor Red
  Write-Host ($_.ScriptStackTrace) -ForegroundColor DarkGray
  $failed = $true
}
finally {
  if ($Hives.Count) { Unload-Hives }
  if ($IsoPath) { Dismount-DiskImage -ImagePath $IsoPath -ErrorAction SilentlyContinue | Out-Null }
  if (Test-Path $Vhd) { Dismount-DiskImage -ImagePath $Vhd -ErrorAction SilentlyContinue | Out-Null }
  Stop-Transcript | Out-Null
}
if ($failed) { exit 1 }
