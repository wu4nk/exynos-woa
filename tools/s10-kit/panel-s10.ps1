<#
  panel-s10.ps1 (v1.0) - Galaxy S10: screen brightness check (MaliG76Wddm 1.0.72.2 + UEFI v0.2.2: AOR dimming on the
  S6E3HA9, notes/beyond1-port.md "Brightness"). Run ON THE PHONE, PowerShell as Administrator, after "gpu-s10.ps1
  update" + reboot:
      .\panel-s10.ps1            what the driver found at start (Panel* trace) + whether Windows offers brightness
      .\panel-s10.ps1 sweep      100 -> 50 -> 10 -> 0 -> 100 %, 3 s each - WATCH the screen (it should dim smoothly
                                 and come back); the AOR written at each step is logged
      .\panel-s10.ps1 -Top 0x14  set Parameters\PanelAorTop (100 % = that AOR; 0 = sboot's own, the darker old
                                 behaviour) - reboot after it
  Writes C:\S10Prov\panel-s10.txt - send it back.
#>
param([Parameter(Position = 0)][ValidateSet('status', 'sweep')][string]$Action = 'status', [string]$Top = '')
$ErrorActionPreference = 'Continue'
$Out = 'C:\S10Prov\panel-s10.txt'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
"panel-s10.ps1 v1.0  $Action  $(Get-Date -Format s)" | Set-Content $Out -Encoding ascii
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }
$Svc = 'HKLM:\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters'
$Dev = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9828\0\Device Parameters\MaliG76Wddm'

if ($Top) {
  $v = [Convert]::ToInt32(($Top -replace '^0x', ''), $(if ($Top -match '^0x') { 16 } else { 10 }))
  if ($v -ne 0 -and ($v -lt 0x14 -or $v -gt 0x0BC8)) { W "PanelAorTop must be 0 or 0x14..0xBC8"; exit 1 }
  Set-ItemProperty $Svc PanelAorTop $v -Type DWord
  W ("PanelAorTop = 0x{0:X4} - reboot" -f $v)
  exit 0
}
$drv = Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.DeviceID -like 'ACPI\SAMS9828\*' } | Select-Object -First 1
W "MaliG76Wddm $($drv.DriverVersion)  (brightness needs 1.0.72.2+)"
$p = Get-ItemProperty $Svc -ErrorAction SilentlyContinue
W ("Parameters: PanelCmdMode {0} PanelBrightness {1} PanelAorTop {2}" -f $p.PanelCmdMode, $p.PanelBrightness, $p.PanelAorTop)
$d = Get-ItemProperty $Dev -ErrorAction SilentlyContinue
foreach ($n in 'PanelCmdMode', 'PanelDeconGlobal', 'PanelDsimConfig', 'PanelIdleOf2000', 'PanelHandoff', 'PanelAorNow',
               'PanelStartLevel', 'PanelAor0', 'PanelAorTopUsed', 'PanelRxUnexpected', 'PanelStep', 'BrightnessQuery',
               'BrightnessLast', 'BrightnessRamp', 'PanelDisplayPower') {
  if ($null -ne $d.$n) { W ("  {0,-18} 0x{1:X8} ({1})" -f $n, [uint32]$d.$n) }
}
W '  (PanelStep 0 = brightness offered; 2 = DECON/DSIM not in the expected state, 3 = DSIM never idle, 5 = B1 read failed, 6 = sboot AOR darker than 0 %)'
$b = Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBrightness -ErrorAction SilentlyContinue | Select-Object -First 1
if ($b) { W "Windows brightness: $($b.CurrentBrightness) % (slider in Settings / quick settings)" } else { W 'Windows offers NO brightness control' }
if ($Action -eq 'sweep') {
  $m = Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBrightnessMethods -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $m) { W 'no WmiMonitorBrightnessMethods - nothing to sweep'; exit 1 }
  $b0 = $b.CurrentBrightness
  foreach ($l in 100, 50, 10, 0, 100) {
    Invoke-CimMethod -InputObject $m -MethodName WmiSetBrightness -Arguments @{ Timeout = 0; Brightness = [byte]$l } | Out-Null
    Start-Sleep -Seconds 3
    $last = [uint32](Get-ItemProperty $Dev -ErrorAction SilentlyContinue).BrightnessLast
    W ("  {0,3} % -> panel level {1}, AOR 0x{2:X4}, {3}" -f $l, ($last -shr 24), (($last -shr 8) -band 0xFFFF), $(if ($last -band 1) { 'WRITE FAILED' } else { 'ok' }))
  }
  Invoke-CimMethod -InputObject $m -MethodName WmiSetBrightness -Arguments @{ Timeout = 0; Brightness = [byte]$b0 } | Out-Null
  W "back to $b0 %"
}
W "log: $Out"
