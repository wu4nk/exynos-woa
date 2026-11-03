<#
  gpu-s10.ps1 (v1.2) - Galaxy S10 GPU stages G1/G2: run the Mali-G76 job test, switch the screen to the
  Mali display driver, then (G2) to the full WDDM adapter with the Mesa UMD (maliumd.dll). Run ON THE PHONE, PowerShell as Administrator, from the kit folder
  that holds out\ (built by build-s10.cmd v1.3+). Needs UEFI beyond1lte-uefi v0.1.3+ (it powers the
  G3D domain and shows the GPU0 device to Windows; v0.1.2 and older hide it).

    .\gpu-s10.ps1 status    what Windows sees: GPU0, which driver, the last probe result, display trace
    .\gpu-s10.ps1 probe     install MaliG76Probe: the GPU runs ONE job (writes c0ffee42 into RAM) and
                            the result goes to C:\Windows\System32\LogFiles\MaliG76Probe-run.txt
    .\gpu-s10.ps1 display   swap to MaliG76Wddm, display-only mode (GpuEnable 0, FullAdapter 0) - reboot
    .\gpu-s10.ps1 full      G2a: copy maliumd.dll (Mesa d3d10umd + panfrost, ARM64) to System32 and switch
                            MaliG76Wddm to the full adapter (FullAdapter 1, GpuEnable 0) - reboot.
                            Mesa draws with softpipe ON THE CPU in this mode (the Mali does no work yet):
                            expect it to be SLOWER than display-only. maliumd.dll is looked for next to this
                            script, in out-gpu\ and in out-gpu\umd\.
    .\gpu-s10.ps1 off       remove both GPU packages; Microsoft Basic Display takes the screen back - reboot

  Every action also writes C:\S10Prov\gpu-report.txt (send that file back).
  Rescue if the screen stays black after "display": boot TWRP, mount the ESP, create
  \S10EDIAG\nogpu.flg on it (GPU0 disappears on the next boot) - or flash beyond1lte-uefi v0.1.2.
  Back from "full": .\gpu-s10.ps1 display (FullAdapter 0 again; maliumd.dll may stay, it is only loaded
  in full mode). If "full" hangs: nogpu.flg from TWRP, boot, run "display", delete nogpu.flg, reboot.
#>
param([Parameter(Position = 0)][ValidateSet('status', 'probe', 'display', 'full', 'off')][string]$Action = 'status')
$ErrorActionPreference = 'Stop'
$Report = 'C:\S10Prov\gpu-report.txt'
New-Item -ItemType Directory -Force (Split-Path $Report) | Out-Null
"gpu-s10.ps1 v1.2  $Action  $(Get-Date -Format s)" | Set-Content $Report -Encoding ascii
function Say($tag, $m, $c) { Write-Host "[$tag] $m" -ForegroundColor $c; "[$tag] $m" | Add-Content $Report }
function Ok($m)   { Say ' OK ' $m Green }
function Info($m) { Say 'INFO' $m Gray }
function Warn($m) { Say 'WARN' $m Yellow }
function Fail($m) { Say 'FAIL' $m Red; exit 1 }
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
      [Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'run PowerShell as Administrator' }

$Out = Join-Path $PSScriptRoot 'out-gpu'                     # build-s10.cmd v1.7 (S10_GPU=1) puts the Mali packages here
if (-not (Test-Path $Out)) { $Out = Join-Path $PSScriptRoot 'out' }
$ProbeLog = "$env:SystemRoot\System32\LogFiles\MaliG76Probe-run.txt"
$SvcParams = 'HKLM:\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters'
$UmdDst = "$env:SystemRoot\System32\maliumd.dll"   # MaliG76Wddm.inf UserModeDriverName = maliumd.dll x4

function Get-Gpu0 { Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ACPI\SAMS9828\*' } | Select-Object -First 1 }

# Delete every driver-store package whose original INF name is $Inf (both GPU drivers bind ACPI\SAMS9828,
# so only one of them may be in the store, or PnP picks by date).
function Remove-Pkg($Inf) {
  $blocks = (pnputil /enum-drivers | Out-String) -split "(\r?\n){2,}"
  foreach ($b in $blocks) {
    if ($b -match [regex]::Escape($Inf) -and $b -match '(oem\d+\.inf)') {
      $pub = $Matches[1]
      pnputil /delete-driver $pub /uninstall /force | Out-Null
      Ok "removed $Inf ($pub)"
    }
  }
}

function Add-Pkg($Name) {
  $inf = Join-Path $Out "$Name\$Name.inf"
  if (-not (Test-Path $inf)) { Fail "no $inf - build the kit with build-s10.cmd v1.3+ and copy out\ next to this script" }
  $cer = Join-Path $Out 'kit-test-cert.cer'
  if (Test-Path $cer) {
    certutil -addstore -f Root $cer | Out-Null
    certutil -addstore -f TrustedPublisher $cer | Out-Null
  }
  $r = pnputil /add-driver $inf /install 2>&1 | Out-String
  $r | Add-Content $Report
  if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { Write-Host $r; Fail "pnputil $Name ($LASTEXITCODE)" }
  Ok "$Name installed"
}

function Show-Status {
  $g = Get-Gpu0
  if (-not $g) {
    Warn 'GPU0 (ACPI\SAMS9828) is NOT present: UEFI older than v0.1.3, or \S10EDIAG\nogpu.flg on the ESP, or the'
    Warn 'G3D domain did not come up (UEFILOG.TXT on the ESP: lines starting "GpuProbe:")'
    return $false
  }
  $svc = (Get-PnpDeviceProperty -InstanceId $g.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
  $prob = (Get-PnpDeviceProperty -InstanceId $g.InstanceId -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue).Data
  Ok "GPU0 present: $($g.FriendlyName)  status $($g.Status)  driver service '$svc'  problem $prob"
  if (Test-Path $ProbeLog) {
    Info "probe log ($ProbeLog):"
    Get-Content $ProbeLog | ForEach-Object { "    $_" | Add-Content $Report; Write-Host "    $_" }
    $res = Get-Content $ProbeLog | Where-Object { $_ -match 'RESULT:' } | Select-Object -Last 1
    if ($res -match 'PASS') { Ok "probe: $res" } elseif ($res) { Warn "probe: $res" }
  }
  $trace = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($g.InstanceId)\Device Parameters\MaliG76Wddm"
  if (Test-Path $trace) {
    Info 'MaliG76Wddm trace (Device Parameters\MaliG76Wddm):'
    $p = Get-ItemProperty $trace
    $p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object {
      $l = '    {0} = 0x{1:X8}' -f $_.Name, $_.Value; $l | Add-Content $Report; Write-Host $l }
    if ($p.FbMatchesGop -eq 1) { Ok 'display: the framebuffer Windows handed over is the S10 panel (1440x3040)' }
  }
  if (Test-Path $SvcParams) {
    $m = Get-ItemProperty $SvcParams -ErrorAction SilentlyContinue
    Info ("mode registry: FullAdapter {0}  GpuEnable {1}" -f $m.FullAdapter, $m.GpuEnable)
  }
  if (Test-Path $UmdDst) {
    $h = (Get-FileHash $UmdDst -Algorithm SHA256).Hash.ToLower()
    Info ("UMD: $UmdDst  {0:N0} bytes  sha256 {1}" -f (Get-Item $UmdDst).Length, $h)
  } else { Info 'UMD: no maliumd.dll in System32 (only needed for "full")' }
  Get-CimInstance Win32_VideoController | ForEach-Object {
    Info ("video: {0}  {1}x{2}  status {3}" -f $_.Name, $_.CurrentHorizontalResolution, $_.CurrentVerticalResolution, $_.Status) }
  return $true
}

switch ($Action) {
  'status' { Show-Status | Out-Null }
  'probe' {
    if (-not (Show-Status)) { Fail 'no GPU0 - flash beyond1lte-uefi v0.1.3+ first' }
    Remove-Pkg 'maliG76wddm.inf'
    if (Test-Path $ProbeLog) { Remove-Item $ProbeLog -Force }
    Add-Pkg 'MaliG76Probe'
    # The job runs when the device starts (D0 entry), i.e. right after the install; give it a moment.
    for ($i = 0; $i -lt 20 -and -not (Test-Path $ProbeLog); $i++) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $ProbeLog)) {
      Warn 'no probe log yet - reboot the phone, then run: .\gpu-s10.ps1 status'
    } else { Show-Status | Out-Null }
  }
  'display' {
    if (-not (Show-Status)) {
      # Rescue boot (nogpu.flg hides GPU0): nothing to install, but the mode can still be written back so the
      # next boot without the flag starts MaliG76Wddm display-only instead of the full adapter that hung.
      if (-not (Get-Service MaliG76Wddm -ErrorAction SilentlyContinue)) { Fail 'no GPU0 and MaliG76Wddm not installed - flash beyond1lte-uefi v0.1.3+ first' }
      New-Item -Force $SvcParams | Out-Null
      Set-ItemProperty $SvcParams GpuEnable 0 -Type DWord
      Set-ItemProperty $SvcParams FullAdapter 0 -Type DWord
      Ok 'GPU0 hidden (nogpu.flg) - mode written anyway: display-only (GpuEnable 0, FullAdapter 0)'
      Info 'delete \S10EDIAG\nogpu.flg on the ESP (mountvol S: /s, del S:\S10EDIAG\nogpu.flg, mountvol S: /d), then reboot'
      Info "report: $Report"
      exit 0
    }
    Remove-Pkg 'maliG76probe.inf'
    Add-Pkg 'MaliG76Wddm'
    New-Item -Force $SvcParams | Out-Null
    Set-ItemProperty $SvcParams GpuEnable 0 -Type DWord
    Set-ItemProperty $SvcParams FullAdapter 0 -Type DWord
    Ok 'mode: display-only (GpuEnable 0, FullAdapter 0)'
    Info 'reboot, then run: .\gpu-s10.ps1 status   (FbMatchesGop = 1 is the good sign)'
    Info 'black screen after reboot -> TWRP, create \S10EDIAG\nogpu.flg on the ESP (or flash v0.1.2)'
  }
  'full' {
    if (-not (Show-Status)) { Fail 'no GPU0 - flash beyond1lte-uefi v0.1.3+ first' }
    $g = Get-Gpu0
    $svc = (Get-PnpDeviceProperty -InstanceId $g.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
    if ($svc -ne 'MaliG76Wddm') { Fail "GPU0 runs '$svc', not MaliG76Wddm - run .\gpu-s10.ps1 display first, reboot, check status" }
    $src = @((Join-Path $PSScriptRoot 'maliumd.dll'), (Join-Path $Out 'maliumd.dll'), (Join-Path $Out 'umd\maliumd.dll')) |
      Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $src) { Fail 'no maliumd.dll next to this script, in out-gpu\ or out-gpu\umd\ - copy it there first' }
    $sum = Join-Path (Split-Path $src) 'SHA256SUMS'
    $h = (Get-FileHash $src -Algorithm SHA256).Hash.ToLower()
    if ((Test-Path $sum) -and -not (Select-String -Path $sum -Pattern $h -SimpleMatch -Quiet)) {
      Fail "maliumd.dll sha256 $h is not in $sum - copy damaged?"
    }
    # A loaded DLL cannot be overwritten but can be renamed: move the old one aside first.
    if (Test-Path $UmdDst) {
      $old = "$UmdDst.old"
      if (Test-Path $old) { Remove-Item $old -Force -ErrorAction SilentlyContinue }
      Rename-Item $UmdDst (Split-Path $old -Leaf) -Force
      Info "previous maliumd.dll -> $old"
    }
    Copy-Item $src $UmdDst -Force
    Ok ("maliumd.dll -> $UmdDst ({0:N0} bytes, sha256 {1})" -f (Get-Item $UmdDst).Length, $h)
    New-Item -Force $SvcParams | Out-Null
    Set-ItemProperty $SvcParams FullAdapter 1 -Type DWord
    Set-ItemProperty $SvcParams GpuEnable 0 -Type DWord
    Ok 'mode: full adapter, GPU half off (FullAdapter 1, GpuEnable 0) - Mesa softpipe draws on the CPU'
    Info 'reboot, then run: .\gpu-s10.ps1 status   and collect logs (s10-log.cmd)'
    Info 'back: .\gpu-s10.ps1 display + reboot.  Hang: TWRP -> \S10EDIAG\nogpu.flg on the ESP, boot, "display", delete the flag'
  }
  'off' {
    Remove-Pkg 'maliG76wddm.inf'
    Remove-Pkg 'maliG76probe.inf'
    Ok 'GPU packages removed - reboot; Microsoft Basic Display drives the screen again'
  }
}
Info "report: $Report"
