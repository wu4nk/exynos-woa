<#
  update-ufs.ps1 (v1.0) - swap the UFS storage driver (Exynos9820Ufs) of a Windows ALREADY running on the
  Galaxy S10 for the one build-s10.cmd made, with no reinstall and no LBA editing - dual boot included.
  Run ON THE PHONE, PowerShell as Administrator, from the kit folder that holds out\:
      Set-ExecutionPolicy -Scope Process Bypass
      .\update-ufs.ps1            # checks, installs, asks to reboot
      .\update-ufs.ps1 -Check     # checks only, installs nothing

  Why no sectors to set: the S10 driver's write fence is cache start .. end of disk (LBA 3118592 .. 31236090,
  4 KiB LBAs), with omr + cp_debug refused inside it. Whatever way userdata was split for dual boot (Android
  userdata + a Windows partition + maybe an own ESP), every new partition sits inside that span, so the driver
  is the same for every layout. This script READS the live layout and checks exactly that before installing:
  the Windows volume and the boot partition must be inside the fence and outside omr / cp_debug, otherwise
  the driver would refuse their writes and it stops.

  Order with UEFI v0.2.0: v0.2.0's DSDT connects the real UFS interrupt (GSIV 281); a driver older than w43
  does not acknowledge it -> interrupt storm at boot. So: this script first (the new driver also runs on the
  old UEFI's parked line, polling), reboot, and only then flash beyond1lte-uefi-v0.2.0.img.
#>
param([switch]$Check)
$ErrorActionPreference = 'Stop'
function Ok($m)   { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }

# Galaxy S10 (beyond1lte) constants, the same ones the driver is built with (Exynos9820Ufs.h, VERIFIED from the
# phone's live GPT 2026-11-02). 4096-byte LBAs.
$FenceFirst = 3118592;  $FenceLast = 31236090          # cache start .. LastUsableLBA
$Refused    = @(@{ n = 'omr';      a = 3272192; b = 3284991 },
                @{ n = 'cp_debug'; a = 3284992; b = 3286271 })

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
      [Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'run PowerShell as Administrator' }
$Dir = Join-Path $PSScriptRoot 'out\Exynos9820Ufs'
$Inf = Join-Path $Dir 'Exynos9820Ufs.inf'
$Sys = Join-Path $Dir 'Exynos9820Ufs.sys'
if (-not (Test-Path $Inf) -or -not (Test-Path $Sys) -or -not (Test-Path (Join-Path $Dir 'Exynos9820Ufs.cat'))) {
  Fail "no $Dir\Exynos9820Ufs.{inf,sys,cat} - run build-s10.cmd (v1.9+) on the PC and copy out\ next to this script"
}

# 1. the .sys must be the S10 build: one "UFSBRD01" record, board 2, fence starting at the S10's cache
$ub  = [IO.File]::ReadAllBytes($Sys)
$txt = [Text.Encoding]::ASCII.GetString($ub)
$at  = $txt.IndexOf('UFSBRD01', [StringComparison]::Ordinal)
if ($at -lt 0 -or $txt.IndexOf('UFSBRD01', $at + 1, [StringComparison]::Ordinal) -ge 0) {
  Fail "$Sys has no (or two) UFSBRD01 board record - build it with build-s10.cmd, not from the .vcxproj"
}
$uBoard = [BitConverter]::ToUInt32($ub, $at + 8); $uFence = [BitConverter]::ToUInt32($ub, $at + 12)
if ($uBoard -ne 2 -or $uFence -ne $FenceFirst) {
  Fail "the .sys is built for board $uBoard (fence LBA $uFence), not the Galaxy S10 (board 2, fence $FenceFirst). An S10e driver here would let Windows write into vendor..optics."
}
$newVer = $null
foreach ($l in Get-Content $Inf) { if ($l -match '^\s*DriverVer\s*=\s*[^,]+,\s*(\d+\.\d+\.\d+\.\d+)') { $newVer = [version]$Matches[1] } }
Ok "new driver: $newVer, built for the Galaxy S10 (fence $FenceFirst..$FenceLast)"

# 2. the driver running now
$cur = Get-CimInstance Win32_PnPSignedDriver -Filter "InfName LIKE 'oem%'" -ErrorAction SilentlyContinue |
       Where-Object { $_.DeviceName -match 'UFS' -or $_.HardWareID -match 'UFS' -or $_.Description -match 'Exynos.*UFS' } |
       Select-Object -First 1
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='Exynos9820Ufs'" -ErrorAction SilentlyContinue
if ($cur) { Ok "running now: $($cur.DriverVersion) ($($cur.InfName)), service $($svc.State)" }
elseif ($svc) { Ok "running now: service Exynos9820Ufs $($svc.State) (version not listed)" }
else { Warn 'could not find the running UFS driver (installing anyway: pnputil adds it for ACPI\UFS0)' }
if ($cur -and $newVer -and [version]$cur.DriverVersion -ge $newVer) {
  Warn "installed $($cur.DriverVersion) is not older than $newVer - Windows keeps the old one. build-s10.cmd v1.9 stamps a newer DriverVer; rebuild if this is an old out\."
}

# 3. live layout: the volumes Windows writes must be inside the fence (this is all dual boot can change)
$disk = Get-Partition -DriveLetter ($env:SystemDrive.TrimEnd(':')) | Get-Disk
$ss   = [int64]$disk.LogicalSectorSize
if ($ss -ne 4096) { Fail "disk $($disk.Number) has $ss-byte sectors, the S10's UFS has 4096 - wrong disk?" }
$esp  = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$bad  = 0
foreach ($p in Get-Partition -DiskNumber $disk.Number) {
  $first = [int64]($p.Offset / $ss); $last = $first + [int64]($p.Size / $ss) - 1
  $role = @()
  if ($p.DriveLetter -and "$($p.DriveLetter):" -eq $env:SystemDrive) { $role += 'Windows' }
  if ($p.IsSystem -or $p.GptType -eq $esp) { $role += 'boot (ESP)' }
  if ($p.DriveLetter -and -not $role) { $role += "drive $($p.DriveLetter):" }
  if (-not $role) { continue }        # Android partitions: Windows has no volume there, never writes them
  $what = "{0} = partition {1}, LBA {2}..{3}" -f ($role -join ' + '), $p.PartitionNumber, $first, $last
  $hit  = $Refused | Where-Object { $first -le $_.b -and $last -ge $_.a } | ForEach-Object { $_.n }
  if ($first -lt $FenceFirst -or $last -gt $FenceLast -or $hit) {
    $why = if ($hit) { "overlaps $($hit -join ', ')" } else { "outside the fence $FenceFirst..$FenceLast" }
    if ($role -contains 'Windows' -or $role -contains 'boot (ESP)') { Write-Host "[FAIL] $what - $why" -ForegroundColor Red; $bad++ }
    else { Warn "$what - ${why}: writes to it will be refused (reads work)" }
  } else { Ok "$what - inside the fence" }
}
if ($bad) { Fail 'this layout needs a different fence - send the output above (and the GPT from TWRP: sgdisk -p /dev/block/sda) instead of editing LBAs by hand' }

if ($Check) { Ok 'check only (-Check): nothing installed'; exit 0 }

# 4. install
$cer = Join-Path $PSScriptRoot 'out\kit-test-cert.cer'
if (Test-Path $cer) {
  certutil -addstore -f Root $cer | Out-Null; certutil -addstore -f TrustedPublisher $cer | Out-Null
  Ok 'kit test certificate trusted'
}
$r = pnputil /add-driver $Inf /install 2>&1 | Out-String
Write-Host $r.Trim()
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { Fail "pnputil exit $LASTEXITCODE" }
Ok 'UFS driver staged - it takes over at the next boot'
Write-Host ''
Write-Host 'Next: reboot, check Windows comes up, THEN flash beyond1lte-uefi-v0.2.0.img (it needs this driver).' -ForegroundColor Cyan
