<#
  camera-s10.ps1 (v1.4) - Galaxy S10: the front camera as a Windows camera (S10eCamera.sys, AVStream;
  notes/s10-camera.md). Run ON THE PHONE, PowerShell as Administrator, in the kit folder (out\S10eCamera\ from
  build-s10.cmd v1.18+):
      .\camera-s10.ps1 install   install the driver package (pnputil) and create the device it binds to: a
                                 ROOT-ENUMERATED device ROOT\S10eCamera (v1.1; the documented SetupAPI sequence the
                                 WDK's devcon "install" uses - it stays across reboots like any root device). v1.0's
                                 SwDeviceCreate failed with 0x8007007E on the phone. Then open the Camera app.
      .\camera-s10.ps1 status    the device, its driver and problem code, its settings (rotation / mirror / ring) and
                                 the driver's last start/stop report (C:\S10Prov\camera-avs.txt)
      .\camera-s10.ps1 set -Rotation 270 -Mirror 0 -RingWc 1 -RingKeep 1 -SensorBin 1
                                 change the driver's settings (S10eCamera 0.2+, registry Services\S10eCamera\
                                 Parameters) and restart the device so they apply. Only the ones given change.
      .\camera-s10.ps1 remove    remove the device and the driver package
  Do not run cam-s10.ps1 shot/stream while an app uses the camera (both drivers refuse a camera already running).
  Writes C:\S10Prov\camera-s10-<action>.txt - send it back.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'status', 'set', 'remove')][string]$Action = 'status',
      [ValidateSet(-1, 0, 90, 180, 270)][int]$Rotation = -1, [ValidateSet(-1, 0, 1)][int]$Mirror = -1,
      [ValidateSet(-1, 0, 1)][int]$RingWc = -1, [ValidateSet(-1, 0, 1)][int]$RingKeep = -1,
      [ValidateSet(-1, 0, 1)][int]$SensorBin = -1)
$ErrorActionPreference = 'Continue'
$Out = "C:\S10Prov\camera-s10-$Action.txt"
$HwId = 'ROOT\S10eCamera'
$Params = 'HKLM:\SYSTEM\CurrentControlSet\Services\S10eCamera\Parameters'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Remove-Item $Out -Force -ErrorAction SilentlyContinue
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }
W "camera-s10.ps1 v1.4  $Action  $(Get-Date -Format s)"

if (-not ('S10RootDev11' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
// devcon's "install" sequence (WDK tools sample, documented SetupAPI): a device info element of the INF's class with a
// generated ROOT\<CLASS>\nnnn instance, its hardware ID set, registered with DIF_REGISTERDEVICE; then
// UpdateDriverForPlugAndPlayDevices installs the INF's driver on every device with that hardware ID.
public static class S10RootDev11 {
  [StructLayout(LayoutKind.Sequential)] struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }
  [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool SetupDiGetINFClass(string inf, out Guid classGuid, StringBuilder className, int size, out int required);
  [DllImport("setupapi.dll", SetLastError = true)]
  static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid classGuid, IntPtr hwnd);
  [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool SetupDiCreateDeviceInfo(IntPtr set, string name, ref Guid classGuid, string desc, IntPtr hwnd, uint flags, ref SP_DEVINFO_DATA data);
  [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool SetupDiSetDeviceRegistryProperty(IntPtr set, ref SP_DEVINFO_DATA data, uint prop, byte[] buf, uint size);
  [DllImport("setupapi.dll", SetLastError = true)]
  static extern bool SetupDiCallClassInstaller(uint fn, IntPtr set, ref SP_DEVINFO_DATA data);
  [DllImport("setupapi.dll", SetLastError = true)]
  static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr hwnd, string hwid, string inf, uint flags, out bool reboot);
  const uint DICD_GENERATE_ID = 1, SPDRP_HARDWAREID = 1, DIF_REGISTERDEVICE = 0x19, INSTALLFLAG_FORCE = 1;

  public static string Create(string inf, string hwid) {
    Guid cls; StringBuilder name = new StringBuilder(64); int req;
    if (!SetupDiGetINFClass(inf, out cls, name, name.Capacity, out req))
      return "SetupDiGetINFClass failed " + Marshal.GetLastWin32Error();
    IntPtr set = SetupDiCreateDeviceInfoList(ref cls, IntPtr.Zero);
    if (set == new IntPtr(-1)) return "SetupDiCreateDeviceInfoList failed " + Marshal.GetLastWin32Error();
    try {
      SP_DEVINFO_DATA d = new SP_DEVINFO_DATA(); d.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
      if (!SetupDiCreateDeviceInfo(set, name.ToString(), ref cls, null, IntPtr.Zero, DICD_GENERATE_ID, ref d))
        return "SetupDiCreateDeviceInfo failed " + Marshal.GetLastWin32Error();
      byte[] ids = Encoding.Unicode.GetBytes(hwid + "\0\0");
      if (!SetupDiSetDeviceRegistryProperty(set, ref d, SPDRP_HARDWAREID, ids, (uint)ids.Length))
        return "SetupDiSetDeviceRegistryProperty failed " + Marshal.GetLastWin32Error();
      if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref d))
        return "DIF_REGISTERDEVICE failed " + Marshal.GetLastWin32Error();
      return "registered a " + name + " device (class " + cls + ") with hardware ID " + hwid;
    } finally { SetupDiDestroyDeviceInfoList(set); }
  }
  public static string Install(string inf, string hwid) {
    bool reboot;
    if (!UpdateDriverForPlugAndPlayDevices(IntPtr.Zero, hwid, inf, INSTALLFLAG_FORCE, out reboot))
      return "UpdateDriverForPlugAndPlayDevices failed " + Marshal.GetLastWin32Error();
    return "driver installed on " + hwid + (reboot ? " (Windows asks for a reboot)" : "");
  }
}
'@
}

function Get-CamDevice {
  Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
    $_.HardwareID -contains $HwId -or $_.HardwareID -contains 'SWD\S10eCamera' -or $_.HardwareID -contains 'ACPI\SAMS982B' }
}

function Show-Status {
  $d = Get-CamDevice
  if (-not $d) { W 'device: none (run install)' }
  foreach ($x in $d) {
    W ("device: {0}  status {1}  class {2}  '{3}'" -f $x.InstanceId, $x.Status, $x.Class, $x.FriendlyName)
    $p = Get-PnpDeviceProperty -InstanceId $x.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode', 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue
    foreach ($q in $p) { W ("  {0} = {1}" -f $q.KeyName, $q.Data) }
  }
  $cams = Get-PnpDevice -Class Camera -PresentOnly -ErrorAction SilentlyContinue
  W ("cameras present: " + (($cams | ForEach-Object { $_.FriendlyName }) -join ', '))
  $pv = Get-ItemProperty $Params -ErrorAction SilentlyContinue
  if ($pv) { W ("settings: Rotation {0}  Mirror {1}  RingWc {2}  RingKeep {3}  SensorBin {4}  (driver defaults when absent: 270 / 1 / 1 / 1 / 1)" -f $pv.Rotation, $pv.Mirror, $pv.RingWc, $pv.RingKeep, $pv.SensorBin) }
  if (Test-Path 'C:\S10Prov\camera-avs.txt') {
    W '--- C:\S10Prov\camera-avs.txt (last 60 lines) ---'
    Get-Content 'C:\S10Prov\camera-avs.txt' -Tail 60 | ForEach-Object { W $_ }
  }
}

function Restart-Cam {
  foreach ($x in (Get-CamDevice | Where-Object Present)) {
    pnputil /restart-device "$($x.InstanceId)" 2>&1 | ForEach-Object { W "  $_" }
  }
}

switch ($Action) {
  'install' {
    $inf = Join-Path $PSScriptRoot 'out\S10eCamera\S10eCamera.inf'
    if (-not (Test-Path $inf)) { W "no $inf - build with build-s10.cmd v1.18+ and copy out\ here"; exit 1 }
    if (Get-Service S10eCam -ErrorAction SilentlyContinue | Where-Object Status -eq 'Running') { sc.exe stop S10eCam | Out-Null; W 'stopped the S10eCam test service (one camera user at a time)' }
    pnputil /add-driver $inf /install 2>&1 | ForEach-Object { W "  $_" }
    if (Get-CamDevice | Where-Object { $_.HardwareID -contains $HwId }) {
      W "a $HwId device exists already: only updating its driver"
    } else {
      W ([S10RootDev11]::Create($inf, $HwId))
    }
    W ([S10RootDev11]::Install($inf, $HwId))
    Start-Sleep -Seconds 3
    Show-Status
    W 'next: open the Camera app (or Teams / a browser) and pick "Galaxy S10 Front Camera"; then .\camera-s10.ps1 status'
  }
  'status' { Show-Status }
  'set' {
    # never New-Item -Force on an existing key: it REPLACES the key (values and subkeys, e.g. a KMDF driver's
    # Parameters\Wdf) - camera-s10 v1.2 wiped S10eCamera's Rotation / Mirror / RingWc that way (2026-11-17)
    if (-not (Test-Path $Params)) { New-Item -Path $Params -ErrorAction SilentlyContinue | Out-Null }
    if ($Rotation -ge 0) { Set-ItemProperty $Params Rotation $Rotation -Type DWord }
    if ($Mirror -ge 0) { Set-ItemProperty $Params Mirror $Mirror -Type DWord }
    if ($RingWc -ge 0) { Set-ItemProperty $Params RingWc $RingWc -Type DWord }
    if ($RingKeep -ge 0) { Set-ItemProperty $Params RingKeep $RingKeep -Type DWord }   # v1.2, S10eCamera 0.5+
    if ($SensorBin -ge 0) { Set-ItemProperty $Params SensorBin $SensorBin -Type DWord }  # v1.4, S10eCamera 0.6+
    W 'restarting the camera device so the driver reads them (close the Camera app first)'
    Restart-Cam
    Show-Status
  }
  'remove' {
    foreach ($x in (Get-CamDevice)) {
      pnputil /remove-device "$($x.InstanceId)" 2>&1 | ForEach-Object { W "  $_" }
    }
    Start-Sleep -Seconds 2
    $pkgs = pnputil /enum-drivers | Out-String
    foreach ($m in [regex]::Matches($pkgs, '(?ms)Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+s10ecamera\.inf')) {
      pnputil /delete-driver $m.Groups[1].Value /uninstall /force 2>&1 | ForEach-Object { W "  $_" }
    }
    Show-Status
  }
}
W "log: $Out"
