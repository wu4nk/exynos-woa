<#
  collect-logs.ps1 (v1.0) - gather everything needed to debug touch / audio / WiFi / battery / keys
  on the Galaxy S10 into ONE zip. Run ON THE PHONE, PowerShell as Administrator:
      Set-ExecutionPolicy -Scope Process Bypass
      .\collect-logs.ps1
  Output: C:\S10Prov\s10-logs-<date>.zip (send that file). Read-only except for mounting the ESP
  for a moment (mountvol, removed again) to copy UEFILOG.TXT / WINPRAM.BIN.
#>
$ErrorActionPreference = 'Continue'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
      [Security.Principal.WindowsBuiltInRole]::Administrator)) { Write-Host 'run PowerShell as Administrator'; exit 1 }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$root  = 'C:\S10Prov'
$dir   = Join-Path $root "s10-logs-$stamp"
New-Item -ItemType Directory -Force $dir | Out-Null
function Save($name, [scriptblock]$b) {
  try { & $b 2>&1 | Out-File -Encoding utf8 -Width 400 (Join-Path $dir $name) }
  catch { "ERROR: $_" | Out-File -Encoding utf8 (Join-Path $dir $name) }
}

# --- system ---
Save 'system.txt' {
  "collect-logs v1.0  $stamp"
  Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture | Format-List
  Get-CimInstance Win32_BIOS | Select-Object SMBIOSBIOSVersion, Manufacturer | Format-List
  bcdedit /enum '{current}'
}

# --- devices: our ACPI IDs, every device with a problem, driver store ---
$ids = @{ 'SCSY0761' = 'touch'; 'SAMS9827' = 'audio'; 'SAMS9825' = 'wifi'; 'SAMS9826' = 'battery';
          'SAMS9820' = 'i2c'; 'SAMS9821' = 'usb-fn'; 'SAMS9828' = 'gpu' }
Save 'devices.txt' {
  foreach ($k in $ids.Keys) {
    "===== ACPI\$k ($($ids[$k]))"
    $d = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "ACPI\$k\*" }
    if (-not $d) { '  not present (hidden by the firmware or not in the DSDT)'; continue }
    foreach ($x in $d) {
      $x | Format-List FriendlyName, InstanceId, Status, Present, Problem, Class
      Get-PnpDeviceProperty -InstanceId $x.InstanceId -KeyName DEVPKEY_Device_DriverVersion, DEVPKEY_Device_DriverInfPath,
        DEVPKEY_Device_Service, DEVPKEY_Device_ProblemCode, DEVPKEY_Device_ProblemStatus -ErrorAction SilentlyContinue |
        Format-Table KeyName, Data -AutoSize
      $dp = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($x.InstanceId)\Device Parameters"
      if (Test-Path $dp) { 'Device Parameters:'; Get-ItemProperty $dp | Format-List }
    }
  }
}
Save 'problem-devices.txt' { pnputil /enum-devices /problem }
Save 'driver-store.txt'   { pnputil /enum-drivers }
Save 'all-devices.txt'    { Get-PnpDevice | Sort-Object Class | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize }

# --- per-driver state ---
Save 'audio.txt' {
  Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters' -ErrorAction SilentlyContinue | Format-List
  Get-CimInstance Win32_SoundDevice | Format-List Name, Status, ConfigManagerErrorCode
  Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Format-Table Status, FriendlyName, InstanceId -AutoSize
}
Save 'wifi.txt' {
  netsh wlan show drivers
  netsh wlan show interfaces
  Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Format-Table Name, InterfaceDescription, Status, MacAddress -AutoSize
  'bcm4375 folder:'
  Get-ChildItem "$env:SystemRoot\System32\drivers\bcm4375" -ErrorAction SilentlyContinue | Format-Table Name, Length -AutoSize
}
Save 'battery.txt' { Get-CimInstance Win32_Battery | Format-List *; powercfg /batteryreport /output (Join-Path $dir 'battery-report.html') }
Save 'services.txt' { Get-Service S10eAgent, S10eDiag -ErrorAction SilentlyContinue | Format-List Name, Status, StartType }

# --- driver log files ---
$logs = Join-Path $dir 'LogFiles'
New-Item -ItemType Directory -Force $logs | Out-Null
foreach ($p in 'Exynos9820WlanCx*.txt', 'Exynos9820Wlan*.txt', 'Exynos9820Audio*.txt', 'MaliG76*.txt', 'S10e*.txt', 'Exynos9820Ufs*.txt') {
  Copy-Item "$env:SystemRoot\System32\LogFiles\$p" $logs -ErrorAction SilentlyContinue
}
foreach ($p in 'C:\S10Prov\*.log', 'C:\S10Prov\*.txt') { Copy-Item $p $dir -ErrorAction SilentlyContinue }

# --- event logs (last 300 system events: driver load failures show here) ---
Save 'events-system.txt' {
  Get-WinEvent -LogName System -MaxEvents 300 -ErrorAction SilentlyContinue |
    Format-Table TimeCreated, Id, LevelDisplayName, ProviderName, Message -AutoSize -Wrap
}

# --- ESP: UEFILOG.TXT (the firmware's own log: PCIe/WLAN bring-up, touch power, audio probe, GPU) ---
$esp = $null
foreach ($l in 'S', 'R', 'Q', 'P') { if (-not (Test-Path "${l}:\")) { $esp = $l; break } }
if ($esp) {
  mountvol "${esp}:" /s 2>$null
  if (Test-Path "${esp}:\") {
    foreach ($f in 'UEFILOG.TXT', 'WINPRAM.BIN', 'WINPRAM-RESTART.BIN') { Copy-Item "${esp}:\$f" $dir -ErrorAction SilentlyContinue }
    Get-ChildItem "${esp}:\" -Recurse -ErrorAction SilentlyContinue | Select-Object FullName, Length |
      Out-File -Encoding utf8 (Join-Path $dir 'esp-listing.txt')
    mountvol "${esp}:" /d 2>$null
  } else { 'mountvol /s failed' | Out-File (Join-Path $dir 'esp-listing.txt') }
}

$zip = "$dir.zip"
Compress-Archive -Path "$dir\*" -DestinationPath $zip -Force
Write-Host "[ OK ] $zip" -ForegroundColor Green
