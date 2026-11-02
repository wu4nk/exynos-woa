<#
  install-in-windows.ps1 (v1.0) - add the S10 kit v1.1 drivers to a Windows ALREADY installed
  on the Galaxy S10, without reinstalling. Run ON THE PHONE, in PowerShell as Administrator,
  from a folder that holds the kit's out\ (and bcm4375\ for WiFi), e.g. a USB stick in the dock:
      Set-ExecutionPolicy -Scope Process Bypass
      .\install-in-windows.ps1
  Does: removes the old S10e touch package (S6SY761TouchBeyond0, same ACPI\SCSY0761), installs
  SecTsY761Touch, S10eBattery, Exynos9820WlanCx (+ WiFi firmware), and the key agent
  (S10eDiag + S10eAgent, through setup\SetupComplete.cmd). Reboot afterwards.
  Flash UEFI v0.1.2+ first: it declares the touch interrupt the way SecTsY761Touch expects.
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
} else { Warn "no $cer - pnputil may refuse the packages (rebuild with build-s10.cmd v1.1)" }

# 1. the old S10e touch package binds the same hardware ID: remove it first
$old = pnputil /enum-drivers | Out-String
$blocks = $old -split "(\r?\n){2,}"
foreach ($b in $blocks) {
  if ($b -match 's6sy761touchbeyond0\.inf' -and $b -match '(oem\d+\.inf)') {
    $pub = $Matches[1]
    pnputil /delete-driver $pub /uninstall /force | Out-Null
    Ok "removed the old touch package $pub (S6SY761TouchBeyond0)"
  }
}

# 2. driver packages
foreach ($n in 'SecTsY761Touch', 'S10eBattery', 'Exynos9820WlanCx', 'Exynos9820Audio') {
  $inf = Join-Path $Out "$n\$n.inf"
  if (-not (Test-Path $inf)) { Warn "$n not built - skipped"; continue }
  $r = pnputil /add-driver $inf /install 2>&1 | Out-String
  if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { Write-Host $r; Fail "pnputil $n ($LASTEXITCODE)" }
  Ok "$n installed"
}

# 3. WiFi firmware where Exynos9820WlanCx loads it
$fw = Join-Path $PSScriptRoot 'bcm4375'
if (Test-Path (Join-Path $fw 'bcmdhd_sta.bin_b1')) {
  $dst = "$env:SystemRoot\System32\drivers\bcm4375"
  New-Item -ItemType Directory -Force $dst | Out-Null
  Copy-Item (Join-Path $fw '*') $dst -Force
  Ok "WiFi firmware -> $dst"
} else { Warn 'no bcm4375\ - run get-wifi-fw.cmd with the phone in TWRP; WiFi needs it' }

# 4. key agent (same steps as SetupComplete.cmd on a fresh install)
$diag = Join-Path $Out 'S10eDiag'
if (Test-Path (Join-Path $diag 'S10eAgent.exe')) {
  New-Item -ItemType Directory -Force 'C:\S10Prov\S10eDiag' | Out-Null
  Copy-Item (Join-Path $diag '*') 'C:\S10Prov\S10eDiag' -Force
  & cmd.exe /c "`"$(Join-Path $PSScriptRoot 'setup\SetupComplete.cmd')`""
  Ok 'key agent installed (log: C:\S10Prov\setupcomplete.log)'
} else { Warn 'no out\S10eDiag - keys skipped' }

Ok 'done - reboot the phone'
