<#
  tc-diag.ps1 (v1.0) - Galaxy S10, DIAGNOSIS ONLY (AGENTS rule 12): one boot with the UMD's threaded context ON for
  every process, to find out where DWM's thread sits when it deadlocks (2026-11-11: DWM froze, black panel, with
  "tc: driver thread waits > 2 s for the DDI thread to run callback op 4"; the threaded context is off by default
  since UMD 0222-s10.1). Run ON THE PHONE, PowerShell as Administrator, from the kit folder:

    .\tc-diag.ps1 on        swaps in umd-tcdiag\maliumd.dll (UMD 0222-s10.3 + s10-0006: ARM64 snapshot of the DWM
                            thread every 5 s while it is missing, and a rescue after 8 s), sets the machine variables
                            MALIUMD_TC=1, MALIUMD_TC_RESCUE=8, MALIKM_TGSI_DIR=C:\ProgramData\S10e\umd. Reboot.
                            The screen may freeze or go black for ~10 s at the desktop (that is the deadlock) - the
                            rescue should bring it back. If it stays black: wait 2 minutes, then hold power to restart
                            and boot with nogpu.flg (TWRP) as before; run "collect" and "off" from that boot.
    .\tc-diag.ps1 collect   C:\S10Prov\tc-diag.zip (the UMD logs of DWM / LogonUI / every process with "tc:" lines)
    .\tc-diag.ps1 off       the installed maliumd.dll back, the three variables removed. Reboot.
#>
param([Parameter(Position = 0)][ValidateSet('on', 'off', 'collect')][string]$Action = 'collect')
$ErrorActionPreference = 'Stop'
function Ok($m)   { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }
$EnvKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
$Dll = 'C:\Windows\System32\maliumd.dll'
$Prev = "$Dll.tcdiag"
$Logs = 'C:\ProgramData\S10e\umd'
function Own($f) { takeown /f $f /a | Out-Null; icacls $f /grant '*S-1-5-32-544:F' | Out-Null }

switch ($Action) {
  'on' {
    $src = Join-Path $PSScriptRoot 'umd-tcdiag\maliumd.dll'
    if (-not (Test-Path $src)) { Fail "no $src - unzip S10-UMD-tcdiag into the kit folder" }
    if (Test-Path $Prev) { Fail "$Prev exists - an earlier 'on' was not undone: run .\tc-diag.ps1 off first" }
    Own $Dll
    Rename-Item $Dll (Split-Path $Prev -Leaf)
    Copy-Item $src $Dll -Force
    Ok ("maliumd.dll = the diagnosis build (sha256 {0}), the installed one kept as {1}" -f (Get-FileHash $Dll).Hash.ToLower().Substring(0, 16), $Prev)
    New-Item -ItemType Directory -Force $Logs | Out-Null
    icacls $Logs /grant '*S-1-1-0:(OI)(CI)M' /Q | Out-Null; icacls $Logs /grant '*S-1-15-2-1:(OI)(CI)M' /Q | Out-Null
    Get-ChildItem $Logs -Filter 'malikm-*' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Set-ItemProperty $EnvKey MALIUMD_TC '1' -Type String
    Set-ItemProperty $EnvKey MALIUMD_TC_RESCUE '8' -Type String
    Set-ItemProperty $EnvKey MALIKM_TGSI_DIR $Logs -Type String
    Ok 'MALIUMD_TC=1, MALIUMD_TC_RESCUE=8, MALIKM_TGSI_DIR set (machine) - reboot now; after the desktop: .\tc-diag.ps1 collect, then off'
  }
  'off' {
    foreach ($n in 'MALIUMD_TC', 'MALIUMD_TC_RESCUE', 'MALIKM_TGSI_DIR') {
      if ($null -ne (Get-ItemProperty $EnvKey -ErrorAction SilentlyContinue).$n) { Remove-ItemProperty $EnvKey $n; Ok "machine $n removed" }
    }
    if (Test-Path $Prev) {
      Own $Dll; Own $Prev
      $old = "$Dll.tcdiag-new"
      if (Test-Path $old) { Remove-Item $old -Force -ErrorAction SilentlyContinue }
      if (Test-Path $old) { $old = "$Dll.tcdiag-new$(Get-Random)" }
      Rename-Item $Dll (Split-Path $old -Leaf)          # in use by DWM: renamed, not deleted
      Rename-Item $Prev (Split-Path $Dll -Leaf)
      Ok ("maliumd.dll back (sha256 {0}) - reboot" -f (Get-FileHash $Dll).Hash.ToLower().Substring(0, 16))
    } else { Warn "no $Prev - maliumd.dll left as it is" }
  }
  'collect' {
    $d = Join-Path $env:TEMP 'tc-diag'
    Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $d | Out-Null
    $n = 0
    foreach ($f in Get-ChildItem $Logs -Filter 'malikm-*' -File -ErrorAction SilentlyContinue) {
      if ($f.Name -match '^malikm-(dwm|LogonUI)\.exe-' -or (Select-String -Path $f.FullName -Pattern 'tc: ' -SimpleMatch -Quiet)) {
        Copy-Item $f.FullName $d; $n++
      }
    }
    Get-ItemProperty $EnvKey | Select-Object MALIUMD_TC, MALIUMD_TC_RESCUE, MALIKM_TGSI_DIR | Out-String | Set-Content (Join-Path $d 'env.txt')
    if (Test-Path $Dll) { "maliumd.dll sha256 $((Get-FileHash $Dll).Hash)" | Add-Content (Join-Path $d 'env.txt') }
    Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1002; StartTime = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime } -ErrorAction SilentlyContinue |
      Select-Object -First 30 | ForEach-Object { "{0:s} {1} {2}" -f $_.TimeCreated, $_.Id, (($_.Message -split "`n")[0..1] -join ' ') } |
      Set-Content (Join-Path $d 'app-errors.txt')
    Remove-Item 'C:\S10Prov\tc-diag.zip' -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force 'C:\S10Prov' | Out-Null
    Compress-Archive -Path "$d\*" -DestinationPath 'C:\S10Prov\tc-diag.zip'
    Ok "$n log(s) -> C:\S10Prov\tc-diag.zip (send it)"
  }
}
