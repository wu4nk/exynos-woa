<#
  install-in-windows.ps1 (v1.1) - add the S10 kit drivers to a Windows ALREADY installed on the
  Galaxy S10, without reinstalling. Run ON THE PHONE, in PowerShell as Administrator, from the kit
  folder that holds out\ (and bcm4375\ for WiFi), e.g. a USB stick in the dock:
      Set-ExecutionPolicy -Scope Process Bypass
      .\install-in-windows.ps1
  Does: removes the older touch packages (S6SY761TouchBeyond0, SecTsY761Touch: same ACPI\SCSY0761), installs
  S10Touch (new touch driver), S10eBattery, Exynos9820WlanCx (+ WiFi firmware), Exynos9820Audio (speaker
  hardware switched ON), and the key agent (S10eDiag + S10eAgent, through setup\SetupComplete.cmd).
  Reboot afterwards. Flash UEFI v0.1.5+ first.
  v1.1: audio Parameters forced to 1 (the INF value does not overwrite an old 0); WiFi firmware set
  checked by name before copying; WLAN / audio device state printed at the end.
#>
$ErrorActionPreference = 'Stop'
function Ok($m)   { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
      [Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'run PowerShell as Administrator' }
$Out = Join-Path $PSScriptRoot 'out'
if (-not (Test-Path $Out)) { Fail "no $Out - run build-s10.cmd on the PC and copy out\ next to this script" }

# 0. trust the kit's test certificate (the drivers are signed with it; test signing is on)
$cer = Join-Path $Out 'kit-test-cert.cer'
if (Test-Path $cer) {
  certutil -addstore -f Root $cer | Out-Null
  certutil -addstore -f TrustedPublisher $cer | Out-Null
  Ok 'kit test certificate trusted (Root + TrustedPublisher)'
} else { Warn "no $cer - pnputil may refuse the packages (rebuild with build-s10.cmd v1.1+)" }

# 1. packages that bind the same hardware IDs as ours: remove them first
$blocks = (pnputil /enum-drivers | Out-String) -split "(\r?\n){2,}"
foreach ($old in 's6sy761touchbeyond0.inf', 'sectsy761touch.inf', 'malig76wddm.inf', 'malig76probe.inf') {
  foreach ($b in $blocks) {
    if ($b -match [regex]::Escape($old) -and $b -match '(oem\d+\.inf)') {
      $pub = $Matches[1]
      pnputil /delete-driver $pub /uninstall /force | Out-Null
      Ok "removed $old ($pub)"
    }
  }
}

# 2. WiFi firmware where Exynos9820WlanCx loads it (WlanDl.c: \SystemRoot\System32\drivers\bcm4375\,
#    firmware bcmdhd_sta.bin_b1, nvram picked by the module's OTP vendor id: Murata 1rh_es43 or Semco CS01)
$fw = Join-Path $PSScriptRoot 'bcm4375'
$fwOk = $false
if (Test-Path (Join-Path $fw 'bcmdhd_sta.bin_b1')) {
  $nv = @('nvram.txt_1rh_es43_b1', 'nvram.txt_CS01_semco_b1') | Where-Object { Test-Path (Join-Path $fw $_) }
  if (-not $nv) { Warn 'bcm4375\ has no nvram.txt_1rh_es43_b1 / nvram.txt_CS01_semco_b1 - WiFi will not start' }
  $dst = "$env:SystemRoot\System32\drivers\bcm4375"
  New-Item -ItemType Directory -Force $dst | Out-Null
  Copy-Item (Join-Path $fw '*') $dst -Force
  Ok "WiFi firmware -> $dst ($((Get-ChildItem $dst).Count) files; nvram: $($nv -join ', '))"
  $fwOk = $true
} else {
  Warn 'no bcm4375\bcmdhd_sta.bin_b1 - run get-wifi-fw.cmd on the PC with the phone in TWRP, copy bcm4375\ here'
  if (Test-Path "$env:SystemRoot\System32\drivers\bcm4375\bcmdhd_sta.bin_b1") { Ok 'firmware already in System32\drivers\bcm4375'; $fwOk = $true }
}

# 3. audio: hardware stages ON (stage D). Written before the install so the first start already plays.
$ap = 'HKLM:\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters'
New-Item -Force $ap | Out-Null
foreach ($v in 'AmpInit', 'AmpEnable', 'AboxEnable') { Set-ItemProperty $ap $v 1 -Type DWord }
Ok 'audio: AmpInit / AmpEnable / AboxEnable = 1'

# 4. driver packages
foreach ($n in 'S10Touch', 'S10eBattery', 'Exynos9820WlanCx', 'Exynos9820Audio') {
  $inf = Join-Path $Out "$n\$n.inf"
  if (-not (Test-Path $inf)) { Warn "$n not built - skipped"; continue }
  $r = pnputil /add-driver $inf /install 2>&1 | Out-String
  # 3010 = reboot needed; 259 = package staged but no present device took it (e.g. WiFi hidden by the firmware)
  if ($LASTEXITCODE -eq 259) { Warn "$n staged in the driver store, no device bound yet"; continue }
  if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { Write-Host $r; Fail "pnputil $n ($LASTEXITCODE)" }
  Ok "$n installed"
}
# the audio package may have been installed earlier with the old 0 values: set them again
foreach ($v in 'AmpInit', 'AmpEnable', 'AboxEnable') { Set-ItemProperty $ap $v 1 -Type DWord }

# 5. key agent (same steps as SetupComplete.cmd on a fresh install)
$diag = Join-Path $Out 'S10eDiag'
if (Test-Path (Join-Path $diag 'S10eAgent.exe')) {
  New-Item -ItemType Directory -Force 'C:\S10Prov\S10eDiag' | Out-Null
  Copy-Item (Join-Path $diag '*') 'C:\S10Prov\S10eDiag' -Force
  & cmd.exe /c "`"$(Join-Path $PSScriptRoot 'setup\SetupComplete.cmd')`""
  Ok 'key agent installed (log: C:\S10Prov\setupcomplete.log)'
} else { Warn 'no out\S10eDiag - keys skipped' }

# 5b. kit watcher: at every logon, a new kit left on the ESP by push-to-windows.cmd is offered for install
$kw = Join-Path $PSScriptRoot 'kit-watch.ps1'
if (Test-Path $kw) {
  Copy-Item $kw 'C:\S10Prov\kit-watch.ps1' -Force
  try {
    $act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\S10Prov\kit-watch.ps1'
    $trg = New-ScheduledTaskTrigger -AtLogOn
    $prn = New-ScheduledTaskPrincipal -GroupId 'BUILTIN\Users' -RunLevel Highest
    $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 30)
    Register-ScheduledTask -TaskName 'S10 kit watcher' -Action $act -Trigger $trg -Principal $prn -Settings $set -Force | Out-Null
    Ok 'kit watcher: every logon offers a new ESP:\S10KIT for install (task "S10 kit watcher")'
  } catch { Warn "kit watcher task: $($_.Exception.Message)" }
} else { Warn 'no kit-watch.ps1 next to this script - no install prompt at logon' }

# 5b. newer helper scripts from the kit onto the desktop / next to the kit copy (s10-log.cmd, gpu-s10.ps1)
foreach ($n in 's10-log.cmd', 'gpu-s10.ps1') {
  $src = Join-Path $PSScriptRoot $n
  if (-not (Test-Path $src)) { continue }
  $dst = if ($n -eq 's10-log.cmd') { Join-Path $env:PUBLIC "Desktop\$n" } else { "C:\S10Prov\kit\$n" }
  if ((Resolve-Path $src).Path -ne $dst) {
    try { Copy-Item $src $dst -Force; Ok "$n -> $dst" } catch { Warn "$n -> ${dst}: $($_.Exception.Message)" }
  }
}

# 6. what Windows sees now
function Show($id, $what) {
  $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "ACPI\$id\*" } | Select-Object -First 1
  if (-not $d) { Warn "$what ($id): not present - the firmware did not expose it (UEFILOG.TXT on the ESP says why)"; return }
  Ok ("{0} ({1}): {2}, status {3}" -f $what, $id, $d.FriendlyName, $d.Status)
}
Show 'SCSY0761' 'touch'
Show 'SAMS9827' 'audio'
Show 'SAMS9825' 'WiFi'
Show 'SAMS9826' 'battery'
if (-not $fwOk) { Warn 'WiFi firmware missing: the WiFi adapter cannot start until bcm4375\ is installed' }

Ok 'done - drivers were replaced live; restart only if a device above is not OK. Logs: s10-log.cmd on the desktop'
exit 0
