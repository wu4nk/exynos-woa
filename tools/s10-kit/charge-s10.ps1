<#
  charge-s10.ps1 (v1.0) - Galaxy S10 charging under Windows (S10eBattery 1.0.0.10+, notes/battery-driver.md).
  Run ON THE PHONE, PowerShell as Administrator, in the kit folder:
      .\charge-s10.ps1 install          put out\S10eBattery (1.0.0.10) in place of the running battery driver
      .\charge-s10.ps1 watch            every 2 s for -Seconds (default 90): charger present / charging, the charger
                                        type the USB-C chip detected (BC1.2: none / USB port / CDP / wall charger),
                                        input limit and charge current in the chip, battery current, temperature,
                                        what ChargeTune decided. Plug / unplug the charger while it runs.
      .\charge-s10.ps1 tune on|off      ChargeTune 1 = the driver sets the input limit / charge current per charger
                                        type from Samsung's S10 table (wall charger 1800 / 2100 mA, USB port
                                        475 / 550, CDP 1500 / 1500; battery >= 50 C -> 500 / 100); 0 = read only.
                                        Applies within ~20 s, no restart.
  Writes C:\S10Prov\charge-s10-<action>.txt - send it back.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'watch', 'tune')][string]$Action = 'watch',
      [Parameter(Position = 1)][ValidateSet('on', 'off', '')][string]$Opt = '',
      [ValidateRange(10, 1800)][int]$Seconds = 90)
$ErrorActionPreference = 'Continue'
$Out = "C:\S10Prov\charge-s10-$Action.txt"
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Remove-Item $Out -Force -ErrorAction SilentlyContinue
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }
W "charge-s10.ps1 v1.0  $Action $Opt  $(Get-Date -Format s)"
$Params = 'HKLM:\SYSTEM\CurrentControlSet\Services\S10eBattery\Parameters'

function Get-DevParams {
  $e = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.Service -eq 'S10eBattery' } | Select-Object -First 1
  if (-not $e) { return $null }
  Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($e.PNPDeviceID)\Device Parameters" -ErrorAction SilentlyContinue
}
function Show-Driver {
  $d = Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.InfName -and $_.DeviceName -match 'Battery' -and $_.DriverProviderName -notmatch 'Microsoft' }
  foreach ($x in $d) { W ("driver: {0} {1}" -f $x.DeviceName, $x.DriverVersion) }
  $t = (Get-ItemProperty $Params -ErrorAction SilentlyContinue).ChargeTune
  W ("ChargeTune setting: {0}" -f $(if ($null -eq $t) { 'absent (= 0, read only)' } else { $t }))
}
function U8($v) { [int]([int64]$v -band 0xFF) }
function U32($v) { [uint32]([int64]$v -band 0xFFFFFFFFL) }   # registry DWORDs come back signed

switch ($Action) {
  'install' {
    $inf = Join-Path $PSScriptRoot 'out\S10eBattery\S10eBattery.inf'
    if (-not (Test-Path $inf)) { W "no $inf - build with build-s10.cmd (EWDK) and copy out\ here"; exit 1 }
    pnputil /add-driver $inf /install 2>&1 | ForEach-Object { W "  $_" }
    Start-Sleep -Seconds 5
    Show-Driver
  }
  'tune' {
    if ($Opt -eq '') { W 'say on or off: .\charge-s10.ps1 tune on'; exit 1 }
    New-Item -Path $Params -Force -ErrorAction SilentlyContinue | Out-Null
    Set-ItemProperty $Params ChargeTune $(if ($Opt -eq 'on') { 1 } else { 0 }) -Type DWord
    Show-Driver
    W 'the driver reads it at its next full reading (<= 20 s)'
  }
  'watch' {
    Show-Driver
    W 'time      ac  state        type       ILIM   CC     batt mA  SOC    temp   tune(target ILIM/CC hot writes)  CHG_DTLS'
    $last = -1
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
      $p = Get-DevParams
      $b = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($p) {
        $cd = U32 $p.ChgDetails
        $chgin = ((U8 $cd) -shr 5) -band 3                # 3 = VBUS valid
        $dtls = ((($cd -shr 8) -band 0xFF) -band 0x0F)    # CHG_DTLS
        $st0 = U32 $p.UsbcSt0
        $bc = if ($st0 -eq [uint32]4294967295) { -1 } else { [int](($st0 -shr 16) -band 0xFF) }
        $type = switch ($bc -band 3) { 0 { 'none' } 1 { 'USB port' } 2 { 'CDP' } 3 { 'wall chg' } }
        if ($bc -lt 0) { $type = '?' }
        $ilim = ((U8 $p.ChgCnfg09) -band 0x7F) + 1; $ilim *= 25
        $ccr = (U8 $p.ChgCnfg02) -band 0x3F; $cc = if ($ccr -le 2) { 100 } else { $ccr * 50 }
        $state = switch ($dtls) { 0 { 'precharge' } 1 { 'fast CC' } 2 { 'fast CV' } 3 { 'top-off' } 4 { 'done' } 8 { 'off' } 0xB { 'WDT expired' } default { "dtls $dtls" } }
        $ac = if ($chgin -eq 3) { 'yes' } else { 'no ' }
        $mark = if ($p.PollOk -ne $last) { '*' } else { ' ' }   # * = a new full reading since the last line
        $last = $p.PollOk
        W ("{0}{1} {2} {3,-12} {4,-9} {5,4}  {6,4}  {7,7}  {8,5:N1}  {9,4:N1}C  {10}({11}/{12} {13} {14})  {15}" -f `
          (Get-Date -Format 'HH:mm:ss'), $mark, $ac, $state, $type, $ilim, $cc, [int]$p.AvgCurrentMa, ($p.SocTenths / 10.0),
          ([int]$p.TempMilliC / 1000.0), $p.TuneSetting, $p.TuneIlimMa, $p.TuneCcMa, $p.TuneHot, $p.TuneWrites, $dtls)
      } else {
        W 'no S10eBattery device parameters'
      }
      Start-Sleep -Seconds 2
    }
    if ($b) { W ("Windows: {0} %  BatteryStatus {1} (1 discharging, 2 on AC)" -f $b.EstimatedChargeRemaining, $b.BatteryStatus) }
  }
}
W "log: $Out"
