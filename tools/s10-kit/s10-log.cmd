@echo off
rem ---------------------------------------------------------------------------
rem s10-log.cmd (v1.3) - ONE click on the phone (Windows) after testing: collects everything about touch / I2C /
rem the firmware into C:\log.zip. Then, in TWRP:   adb pull /data/log.zip
rem (TWRP mounting Windows read-only is fine for a pull). Nothing has to be restarted: the touch driver's
rem PRAM is read live with s10ediag.exe.
rem Put it on the Desktop once:  TWRP: adb push s10-log.cmd /cache/   ->  Windows (admin cmd): mountvol S: /s
rem   copy S:\s10-log.cmd %PUBLIC%\Desktop\   ->  mountvol S: /d
rem ---------------------------------------------------------------------------
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
set "L="
for /f "delims=:" %%N in ('findstr /n /x /c:"#__PS__" "%~f0"') do set "L=%%N"
more +%L% "%~f0" > "%TEMP%\s10-log.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%TEMP%\s10-log.ps1"
del "%TEMP%\s10-log.ps1" >nul 2>&1
echo.
echo Xong. Vao TWRP:  adb pull /data/log.zip
pause
exit /b 0
#__PS__
$ErrorActionPreference = 'Continue'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir   = "C:\S10Prov\logs\$stamp"
New-Item -ItemType Directory -Force $dir | Out-Null
function Save($name, [scriptblock]$b) { try { & $b 2>&1 | Out-File -Encoding utf8 -Width 400 (Join-Path $dir $name) } catch { "ERROR: $_" | Out-File (Join-Path $dir $name) } }
Write-Host "s10-log v1.3 -> $dir"

# 0. S10eDiag (kernel helper, needed for the PRAM read below and for the keys / Reboot to TWRP) and S10eAgent:
#    state, config, logs; a stopped service is started here and the exact error is recorded.
Save 'services.txt' {
  foreach ($svc in 'S10eDiag', 'S10eAgent') {
    "===== $svc"
    sc.exe query $svc
    sc.exe qc $svc
    $st = "$(sc.exe query $svc | Select-String 'STATE')"
    if ($st -notmatch 'RUNNING') {
      "--- not running: sc start $svc"
      sc.exe start $svc
      "exit code $LASTEXITCODE  (2 = file missing, 577 = signature rejected, 1060 = service not installed, 1275 = driver blocked)"
      Start-Sleep 2
      sc.exe query $svc
    }
  }
  '===== files'
  foreach ($f in 'C:\Windows\System32\drivers\S10eDiag.sys', 'C:\Windows\System32\S10eAgent.exe', 'C:\Windows\System32\s10ediag.exe',
                 'C:\Windows\System32\s10ereboot.exe', 'C:\S10Prov\S10eDiag\S10eDiag.sys') {
    if (Test-Path $f) { $s = Get-AuthenticodeSignature $f; "{0}  {1} B  signer: {2}  status: {3}" -f $f, (Get-Item $f).Length, $s.SignerCertificate.Subject, $s.Status } else { "$f  MISSING" }
  }
  '===== bcdedit (testsigning)'
  bcdedit /enum '{current}' | Select-String -Pattern 'testsigning|nointegritychecks'
  '===== \\.\S10eDiag'
  s10ediag.exe info
}
foreach ($f in 'C:\S10Prov\setupcomplete.log', 'C:\Windows\Temp\S10eAgent.log', 'C:\ProgramData\S10e\buttons.ini', 'C:\S10Prov\install.log') {
  Copy-Item $f $dir -ErrorAction SilentlyContinue
}
Write-Host '[ OK ] services'

# 1. live PRAM: S10Touch (+0xE000, 4 KiB) and the S10eDiag mark ring (+0xA000, 16 KiB), decoded to text
$diag = Get-Command s10ediag.exe -ErrorAction SilentlyContinue
function Pram($addr, $len, $name) {
  if (-not $diag) { "no s10ediag.exe (S10eDiag not installed)" | Out-File (Join-Path $dir "$name.txt"); return }
  $hex = & s10ediag.exe peek $addr $len 2>&1
  $hex | Out-File -Encoding ascii (Join-Path $dir "$name-hex.txt")
  $bytes = New-Object System.Collections.Generic.List[byte]
  foreach ($line in $hex) {
    if ("$line" -notmatch '^[0-9a-fA-F]{10}:') { continue }
    foreach ($w in ("$line" -split ':')[1].Trim() -split '\s+') {
      if ($w -notmatch '^[0-9a-fA-F]{8}$') { continue }
      $v = [Convert]::ToUInt32($w, 16)
      0..3 | ForEach-Object { $bytes.Add([byte](($v -shr (8 * $_)) -band 0xFF)) }
    }
  }
  $txt = -join ($bytes | ForEach-Object { if ($_ -eq 10) { "`n" } elseif ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
  ($txt -split "`n" | Where-Object { $_.Trim('. ') }) | Out-File -Encoding utf8 (Join-Path $dir "$name.txt")
}
Pram '0x8FE0E000' 4096  'touch-pram'
Pram '0x8FE0A000' 16384 'diag-marks'
Write-Host '[ OK ] PRAM'

# 2. device state + driver counters (S10Touch S10t*, HSI2C telemetry, everything in Device Parameters)
Save 'devices.txt' {
  foreach ($id in 'SCSY0761', 'SAMS9820', 'SAMS9827', 'SAMS9825', 'SAMS9826', 'SAMS9828') {
    "===== ACPI\$id"
    foreach ($d in @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "ACPI\$id\*" })) {
      $d | Format-List FriendlyName, InstanceId, Status, Problem
      Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion, DEVPKEY_Device_ProblemStatus -ErrorAction SilentlyContinue | Format-Table KeyName, Data -AutoSize
      $dp = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
      if (Test-Path $dp) { Get-ItemProperty $dp | Format-List }
      Get-ChildItem $dp -Recurse -ErrorAction SilentlyContinue | ForEach-Object { $_.Name; Get-ItemProperty $_.PSPath | Format-List }
    }
  }
}
Save 'problem-devices.txt' { pnputil /enum-devices /problem }
Save 'drivers.txt' { pnputil /enum-drivers }
Save 'events-system.txt' { wevtutil qe System /c:300 /rd:true /f:text }
# v1.3 (G2 full mode): who crashed - dwm / LogonUI / winlogon / the UMD (Application Error 1000, WER 1001, Desktop
# Window Manager 9020/9027...), the WER report texts, and the GPU mode the KMD was started with.
Save 'events-application.txt' { wevtutil qe Application /c:300 /rd:true /f:text }
Save 'gpu-mode.txt' {
  '===== Services\MaliG76Wddm\Parameters'
  Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters' -ErrorAction SilentlyContinue | Format-List
  '===== UMD files'
  foreach ($f in "$env:SystemRoot\System32\maliumd.dll", "$env:SystemRoot\System32\maliumd.dll.old") {
    if (Test-Path $f) { "{0}  {1} B  {2}  sha256 {3}" -f $f, (Get-Item $f).Length, (Get-Item $f).LastWriteTime.ToString('s'), (Get-FileHash $f).Hash.ToLower() } else { "$f  -" }
  }
  '===== video controllers'
  Get-CimInstance Win32_VideoController | Format-List Name, Status, PNPDeviceID, InstalledDisplayDrivers, DriverVersion, CurrentHorizontalResolution, CurrentVerticalResolution
}
$wer = Join-Path $dir 'wer'; New-Item -ItemType Directory -Force $wer | Out-Null
foreach ($root in 'C:\ProgramData\Microsoft\Windows\WER\ReportArchive', 'C:\ProgramData\Microsoft\Windows\WER\ReportQueue') {
  Get-ChildItem $root -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'dwm|LogonUI|winlogon|explorer|maliumd|d3d|dxgi|BlueScreen|LiveKernel' } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 20 | ForEach-Object {
      Copy-Item (Join-Path $_.FullName 'Report.wer') (Join-Path $wer "$($_.Name).wer.txt") -ErrorAction SilentlyContinue }
}
Get-ChildItem C:\Windows\LiveKernelReports -Recurse -File -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime |
  Out-File -Encoding utf8 (Join-Path $wer 'livekernelreports.txt')
Copy-Item C:\Windows\INF\setupapi.dev.log $dir -ErrorAction SilentlyContinue
Copy-Item C:\Windows\System32\LogFiles\Exynos*.txt, C:\Windows\System32\LogFiles\S10*.txt, C:\Windows\System32\LogFiles\MaliG76*.txt, C:\S10Prov\gpu-report.txt $dir -ErrorAction SilentlyContinue
Write-Host '[ OK ] devices, counters, events'

# 3. firmware log + saved PRAM dumps from the ESP
$esp = @('S', 'R', 'Q', 'P') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
if ($esp) {
  mountvol "${esp}:" /s 2>$null
  if (Test-Path "${esp}:\") {
    foreach ($f in 'UEFILOG.TXT', 'WINPRAM.BIN', 'WINPRAM-RESTART.BIN') { Copy-Item "${esp}:\$f" $dir -ErrorAction SilentlyContinue }
    Copy-Item "${esp}:\S10EDIAG\out\agent.log" (Join-Path $dir 'esp-agent.log') -ErrorAction SilentlyContinue
    mountvol "${esp}:" /d 2>$null
    Write-Host '[ OK ] UEFILOG / WINPRAM from the ESP'
  }
}

# 4. one zip at the root of C: (= /data/log.zip in TWRP)
Remove-Item C:\log.zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$dir\*" -DestinationPath C:\log.zip -Force
Write-Host "[ OK ] C:\log.zip ($([math]::Round((Get-Item C:\log.zip).Length / 1KB)) KB)" -ForegroundColor Green
