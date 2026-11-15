<#
  mic-s10.ps1 (v1.0) - Galaxy S10: turn the built-in microphones on and check them (Exynos9820Audio 1.0.3.7;
  notes/d8-mic.md, notes/beyond1-port.md "Microphones").
  Why there was no microphone: the driver only builds its "Microphone" endpoint when Parameters\CodecEnable = 1 (the
  CS47L93 codec the mics hang on), and neither SetupComplete nor install-in-windows ever set it - the S10's codec
  wiring is the S10e's (live DT: cs47l93@0 on spi@10470000, dmic-ref <0 0 1 1>), where the mics were measured working.
  Run ON THE PHONE, PowerShell as Administrator, in the kit folder:
      .\mic-s10.ps1          CodecEnable = 1 (+ one 500 ms driver self-test), restart the audio device, list the sound
                             devices, then record 3 s from the microphone - TALK or CLAP while it records
      .\mic-s10.ps1 -Off     back to CodecEnable = 0 (no mic / headphone endpoints)
  Writes C:\S10Prov\mic-s10.txt - send it back. Touches only the audio device (speakers keep working).
#>
param([switch]$Off)
$ErrorActionPreference = 'Continue'
$Out = 'C:\S10Prov\mic-s10.txt'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
"mic-s10.ps1 v1.0  $(if ($Off) { 'off' } else { 'on' })  $(Get-Date -Format s)" | Set-Content $Out -Encoding ascii
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }
$ap = 'HKLM:\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters'
$runlog = 'C:\Windows\System32\LogFiles\Exynos9820Audio-run.txt'
if (-not (Test-Path $ap)) { W "no $ap - the audio driver is not installed"; exit 1 }

$dev = Get-PnpDevice -InstanceId 'ACPI\SAMS9827\*' -ErrorAction SilentlyContinue | Select-Object -First 1
$drv = Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.DeviceID -like 'ACPI\SAMS9827\*' } | Select-Object -First 1
W "audio device: $($dev.FriendlyName) [$($dev.Status)] driver $($drv.DriverVersion)"
$p = Get-ItemProperty $ap
W ("before: CodecEnable {0} AboxEnable {1} HpMaster {2} AmpEnable {3} MicGain {4}" -f $p.CodecEnable, $p.AboxEnable, $p.HpMaster, $p.AmpEnable, $p.MicGain)

if ($Off) {
  Set-ItemProperty $ap CodecEnable 0 -Type DWord
} else {
  Set-ItemProperty $ap CodecEnable 1 -Type DWord
  if ($null -eq $p.MicGain) { Set-ItemProperty $ap MicGain 24 -Type DWord }   # +12 dB, the S10e's measured default
  Set-ItemProperty $ap MicTest 500 -Type DWord                                   # one self-test at the next start
}
W "restarting the audio device..."
pnputil /restart-device "$($dev.InstanceId)" 2>&1 | ForEach-Object { W "  $_" }
Start-Sleep -Seconds 6

W '== sound devices'
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Sort-Object FriendlyName |
  ForEach-Object { W ("  {0,-45} {1}" -f $_.FriendlyName, $_.Status) }
W '== driver run log (codec / mic lines)'
if (Test-Path $runlog) {
  Select-String -Path $runlog -Pattern 'Exynos9820Audio 1|Codec|mic|Mic|AboxCapture|WDMA|circuit|Microphone' |
    Select-Object -Last 60 | ForEach-Object { W "  $($_.Line)" }
} else { W "  no $runlog" }
if ($Off) { W "log: $Out"; exit 0 }

$wascap = @((Join-Path $PSScriptRoot 'tools\wascap.exe'), 'C:\S10Prov\kit\tools\wascap.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $wascap) { W 'no tools\wascap.exe in the kit - skip the recording (open Settings > Sound > Input and talk)'; W "log: $Out"; exit 0 }
W ''
W '>>> RECORDING 3 s - TALK or CLAP NOW <<<'
$raw = Join-Path $env:TEMP 'mic-s10.raw'
& $wascap 3 $raw 2>&1 | ForEach-Object { W "  wascap: $_" }
if (Test-Path $raw) {
  $b = [IO.File]::ReadAllBytes($raw)
  $n = [int]($b.Length / 4)
  if ($n -gt 0) {
    $sum = 0.0; $peak = 0.0; $zero = 0
    for ($i = 0; $i -lt $n; $i++) {
      $v = [BitConverter]::ToSingle($b, $i * 4)
      $a = [Math]::Abs($v); if ($a -gt $peak) { $peak = $a }; if ($v -eq 0) { $zero++ }
      $sum += $v * $v
    }
    $rms = [Math]::Sqrt($sum / $n)
    $db = { param($x) if ($x -le 0) { '-inf' } else { '{0:N1}' -f (20 * [Math]::Log10($x)) } }
    W ("level: {0} samples, RMS {1} dBFS, peak {2} dBFS, exact zeros {3:N1} %" -f $n, (& $db $rms), (& $db $peak), (100.0 * $zero / $n))
    W $(if ($peak -eq 0) { 'RESULT: silence (all zeros) - the mic path delivers nothing' }
        elseif ($peak -lt 0.003) { 'RESULT: very quiet - only noise? (talk louder / closer to the bottom mic)' }
        else { 'RESULT: the microphone hears (louder peaks while talking = working)' })
  }
  Remove-Item $raw -Force -ErrorAction SilentlyContinue
}
W "log: $Out"
