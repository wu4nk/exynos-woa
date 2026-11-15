<#
  vibra-s10.ps1 (v1.0) - Galaxy S10: vibration motor test (S10eBattery 1.0.0.8+, IOCTL_S10E_VIBRATE; notes/s10-vibration.md).
  Run ON THE PHONE, PowerShell as Administrator, in the kit folder:
      .\vibra-s10.ps1 install          put out\S10eBattery (1.0.0.8, with Vibra.c) in place of the running battery
                                       driver (pnputil; no reboot, the battery reading restarts by itself)
      .\vibra-s10.ps1                  test: 3 short pulses, then 1 s at half strength - FEEL the phone
      .\vibra-s10.ps1 -Ms 500 -Strength 100     one run: duration 1..10000 ms, strength 1..100 %
      .\vibra-s10.ps1 stop             motor off now
  Writes C:\S10Prov\vibra-s10.txt (what the driver saw at each step) - send it back.
  Touches nothing but the motor path: MAX77705 haptic registers, PWM channel 1, pins gpp3-1 / gpg1-7.
  The driver stops the motor by itself after the duration (max 10 s), also if this script is closed.
#>
param([Parameter(Position = 0)][ValidateSet('test', 'install', 'stop', 'run')][string]$Action = 'test',
      [int]$Ms = 0, [int]$Strength = 100)
$ErrorActionPreference = 'Continue'
$Out = 'C:\S10Prov\vibra-s10.txt'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
if ($Ms -gt 0) { $Action = 'run' }
"vibra-s10.ps1 v1.0  $Action  $(Get-Date -Format s)" | Set-Content $Out -Encoding ascii
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }

if ($Action -eq 'install') {
  $inf = Join-Path $PSScriptRoot 'out\S10eBattery\S10eBattery.inf'
  if (-not (Test-Path $inf)) { W "no $inf - build with build-s10.cmd (EWDK) and copy out\ here"; exit 1 }
  $ver = (Select-String -Path $inf -Pattern '^DriverVer\s*=\s*(.+)$').Matches[0].Groups[1].Value
  W "installing S10eBattery $ver"
  pnputil /add-driver $inf /install 2>&1 | ForEach-Object { W "  $_" }
  Start-Sleep -Seconds 5
  $d = Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.DeviceName -match 'S10e|Battery' -and $_.InfName -and $_.DriverVersion -like '1.0.0.*' } | Select-Object -First 1
  W "running: $($d.DeviceName) $($d.DriverVersion)  (needs 1.0.0.8)"
  $b = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1
  W "battery: $($b.EstimatedChargeRemaining) %"
  W 'next: .\vibra-s10.ps1'
  exit 0
}

if (-not ('S10Vib' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class S10Vib {
  [StructLayout(LayoutKind.Sequential)] public struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid g; public int Flags; public IntPtr Reserved; }
  [DllImport("setupapi.dll", SetLastError = true)] static extern IntPtr SetupDiGetClassDevsW(ref Guid g, IntPtr e, IntPtr h, int f);
  [DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, int i, ref SP_DEVICE_INTERFACE_DATA o);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr s, ref SP_DEVICE_INTERFACE_DATA d, IntPtr det, int sz, out int need, IntPtr x);
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(IntPtr h, uint code, byte[] i, int il, byte[] o, int ol, out int got, IntPtr ov);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  const uint IOCTL = (0x22u << 16) | (2u << 14) | (0x8A3u << 2);   // CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8A3, METHOD_BUFFERED, FILE_WRITE_ACCESS)
  public static string Error = "";
  // returns the raw S10E_VIBRATE_OUT (80 bytes) or null
  public static byte[] Run(int ms, int intensity) {
    Guid g = new Guid("72631e54-78a4-11d0-bcf7-00aa00b7b32a");   // GUID_DEVICE_BATTERY
    IntPtr set = SetupDiGetClassDevsW(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);
    var d = new SP_DEVICE_INTERFACE_DATA(); d.cbSize = Marshal.SizeOf(d);
    for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref d); i++) {
      int need; SetupDiGetDeviceInterfaceDetailW(set, ref d, IntPtr.Zero, 0, out need, IntPtr.Zero);
      IntPtr det = Marshal.AllocHGlobal(need); Marshal.WriteInt32(det, 8);
      if (!SetupDiGetDeviceInterfaceDetailW(set, ref d, det, need, out need, IntPtr.Zero)) { Marshal.FreeHGlobal(det); continue; }
      string path = Marshal.PtrToStringUni(new IntPtr(det.ToInt64() + 4)); Marshal.FreeHGlobal(det);
      IntPtr h = CreateFileW(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
      if (h == new IntPtr(-1)) { Error += path + ": open " + Marshal.GetLastWin32Error() + "; "; continue; }
      byte[] inb = new byte[8]; BitConverter.GetBytes(ms).CopyTo(inb, 0); BitConverter.GetBytes(intensity).CopyTo(inb, 4);
      byte[] outb = new byte[80]; int got;
      bool ok = DeviceIoControl(h, IOCTL, inb, 8, outb, 80, out got, IntPtr.Zero);
      int err = Marshal.GetLastWin32Error(); CloseHandle(h);
      if (ok && got >= 80) return outb;
      Error += path + ": ioctl " + err + "; ";
    }
    return null;
  }
}
'@
}

function Show($b, $label) {
  if (-not $b) { W "$label : no S10eBattery 1.0.0.8 answered ($([S10Vib]::Error)) - run '.\vibra-s10.ps1 install' first"; return $false }
  $u = { param($o) [BitConverter]::ToUInt32($b, $o) }
  $st = [BitConverter]::ToInt32($b, 8)
  $steps = @('', 'map', 'clock check', 'calibrate', 'pins', 'pwm', 'pmic', 'running')
  W ("{0}: step {1} ({2}), status 0x{3:X8}{4}" -f $label, (& $u 4), $steps[[Math]::Min(7, (& $u 4))], $st, $(if ($st -eq 0) { '  OK' } else { '' }))
  W ("   CMU PWM gate 0x{0:X8} (b28 auto / b20 manual / b21 on)  QCH 0x{1:X8}" -f (& $u 12), (& $u 16))
  W ("   PWM TCFG0 0x{0:X8} TCFG1 0x{1:X8} TCON 0x{2:X8} -> 0x{3:X8}" -f (& $u 20), (& $u 24), (& $u 28), (& $u 32))
  W ("   PWM input clock {0:N0} Hz (counts {1}, QPC ticks {2} @ {3:N0} Hz)  TCNT {4} TCMP {5}" -f (& $u 36), (& $u 40), (& $u 44), (& $u 48), (& $u 52), (& $u 56))
  W ("   pins GPP3 CON 0x{0:X8} PUD 0x{1:X8}  GPG1 CON 0x{2:X8} DAT 0x{3:X8}" -f (& $u 60), (& $u 64), (& $u 68), (& $u 72))
  W ("   PMIC MAINCTRL1 0x{0:X2} -> 0x{1:X2}  MCONFIG 0x{2:X2} -> 0x{3:X2}" -f $b[78], $b[76], $b[79], $b[77])
  return ($st -eq 0)
}

switch ($Action) {
  'stop' { [void](Show ([S10Vib]::Run(0, 0)) 'stop') }
  'run'  {
    $i = [Math]::Max(1, [Math]::Min(100, $Strength)) * 100
    [void](Show ([S10Vib]::Run([Math]::Max(1, [Math]::Min(10000, $Ms)), $i)) "run $Ms ms at $Strength %")
  }
  'test' {
    W 'Watch / hold the phone: 3 short pulses, then 1 second at half strength.'
    $ok = Show ([S10Vib]::Run(150, 10000)) 'pulse 1 (150 ms, 100 %)'
    if ($ok) {
      Start-Sleep -Milliseconds 500; [void](Show ([S10Vib]::Run(150, 10000)) 'pulse 2')
      Start-Sleep -Milliseconds 500; [void](Show ([S10Vib]::Run(150, 10000)) 'pulse 3')
      Start-Sleep -Milliseconds 800; [void](Show ([S10Vib]::Run(1000, 5000)) '1 s at 50 %')
      Start-Sleep -Milliseconds 1300
      W 'Did you feel 3 pulses + 1 longer weaker buzz? Tell Claude yes/no (and how strong) and send this file.'
    }
  }
}
W "log: $Out"
