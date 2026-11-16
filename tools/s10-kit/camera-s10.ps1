<#
  camera-s10.ps1 (v1.0) - Galaxy S10: the front camera as a Windows camera (S10eCamera.sys, AVStream;
  notes/s10-camera.md). Run ON THE PHONE, PowerShell as Administrator, in the kit folder (out\S10eCamera\ from
  build-s10.cmd v1.18+):
      .\camera-s10.ps1 install   install the driver package (pnputil) and create the device it binds to: a software
                                 device SWD\S10eCamera (SwDeviceCreate, lifetime "parent present" = it stays after this
                                 script ends). Then open the Camera app.
      .\camera-s10.ps1 status    the device, its driver and problem code, and the driver's last start/stop report
                                 (C:\S10Prov\camera-avs.txt)
      .\camera-s10.ps1 remove    remove the device and the driver package
  Do not run cam-s10.ps1 shot/stream while an app uses the camera (both drivers refuse a camera already running).
  Writes C:\S10Prov\camera-s10-<action>.txt - send it back.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'status', 'remove')][string]$Action = 'status')
$ErrorActionPreference = 'Continue'
$Out = "C:\S10Prov\camera-s10-$Action.txt"
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Remove-Item $Out -Force -ErrorAction SilentlyContinue
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }
W "camera-s10.ps1 v1.0  $Action  $(Get-Date -Format s)"

if (-not ('S10SwDev' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class S10SwDev {
  [StructLayout(LayoutKind.Sequential)] struct CreateInfo {
    public uint cbSize; public IntPtr pszInstanceId; public IntPtr pszzHardwareIds; public IntPtr pszzCompatibleIds;
    public IntPtr pContainerId; public uint CapabilityFlags; public IntPtr pszDeviceDescription; public IntPtr pszDeviceLocation;
    public IntPtr pSecurityDescriptor;
  }
  delegate void CreateCallback(IntPtr hSwDevice, int createResult, IntPtr context, IntPtr deviceInstanceId);
  [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)] static extern int SwDeviceCreate(string enumerator, string parent, ref CreateInfo info,
      uint propCount, IntPtr props, CreateCallback cb, IntPtr ctx, out IntPtr hSwDevice);
  [DllImport("cfgmgr32.dll")] static extern int SwDeviceSetLifetime(IntPtr h, int lifetime);
  [DllImport("cfgmgr32.dll")] static extern void SwDeviceClose(IntPtr h);
  const int LifetimeHandle = 0, LifetimeParentPresent = 1;
  const uint SilentInstall = 2, DriverRequired = 8;
  static ManualResetEvent done = new ManualResetEvent(false);
  static int result; static string instance;
  static CreateCallback keep = (h, r, c, id) => { result = r; instance = id == IntPtr.Zero ? "" : Marshal.PtrToStringUni(id); done.Set(); };
  // persist = true: the device stays (lifetime parent present); false: reclaim it and let it go (remove)
  public static string Run(bool persist) {
    CreateInfo ci = new CreateInfo();
    ci.cbSize = (uint)Marshal.SizeOf(typeof(CreateInfo));
    ci.pszInstanceId = Marshal.StringToHGlobalUni("0");
    ci.pszzHardwareIds = Marshal.StringToHGlobalUni("SWD\\S10eCamera\0\0");
    ci.CapabilityFlags = SilentInstall | DriverRequired;
    ci.pszDeviceDescription = Marshal.StringToHGlobalUni("Galaxy S10 Front Camera");
    IntPtr h;
    done.Reset();
    int hr = SwDeviceCreate("S10eCamera", "HTREE\\ROOT\\0", ref ci, 0, IntPtr.Zero, keep, IntPtr.Zero, out h);
    if (hr != 0) return string.Format("SwDeviceCreate failed 0x{0:X8}", hr);
    if (!done.WaitOne(30000)) { SwDeviceClose(h); return "SwDeviceCreate: no callback in 30 s"; }
    string msg = string.Format("SwDeviceCreate -> 0x{0:X8}, instance {1}", result, instance);
    if (result == 0) {
      int lr = SwDeviceSetLifetime(h, persist ? LifetimeParentPresent : LifetimeHandle);
      msg += string.Format("; lifetime {0} -> 0x{1:X8}", persist ? "parent present (stays)" : "handle (goes away now)", lr);
    }
    SwDeviceClose(h);
    return msg;
  }
}
'@
}

function Show-Status {
  $d = Get-PnpDevice -InstanceId 'SWD\S10eCamera\*' -ErrorAction SilentlyContinue
  if (-not $d) { $d = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'SWD\S10eCamera' -or $_.HardwareID -contains 'ACPI\SAMS982B' } }
  if (-not $d) { W 'device: none (run install)'; return }
  foreach ($x in $d) {
    W ("device: {0}  status {1}  class {2}  '{3}'" -f $x.InstanceId, $x.Status, $x.Class, $x.FriendlyName)
    $p = Get-PnpDeviceProperty -InstanceId $x.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode', 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue
    foreach ($q in $p) { W ("  {0} = {1}" -f $q.KeyName, $q.Data) }
  }
  $cams = Get-PnpDevice -Class Camera -PresentOnly -ErrorAction SilentlyContinue
  W ("cameras present: " + (($cams | ForEach-Object { $_.FriendlyName }) -join ', '))
  if (Test-Path 'C:\S10Prov\camera-avs.txt') {
    W '--- C:\S10Prov\camera-avs.txt (last 60 lines) ---'
    Get-Content 'C:\S10Prov\camera-avs.txt' -Tail 60 | ForEach-Object { W $_ }
  }
}

switch ($Action) {
  'install' {
    $inf = Join-Path $PSScriptRoot 'out\S10eCamera\S10eCamera.inf'
    if (-not (Test-Path $inf)) { W "no $inf - build with build-s10.cmd v1.18+ and copy out\ here"; exit 1 }
    if (Get-Service S10eCam -ErrorAction SilentlyContinue | Where-Object Status -eq 'Running') { sc.exe stop S10eCam | Out-Null; W 'stopped the S10eCam test service (one camera user at a time)' }
    pnputil /add-driver $inf /install 2>&1 | ForEach-Object { W "  $_" }
    W ([S10SwDev]::Run($true))
    Start-Sleep -Seconds 3
    Show-Status
    W 'next: open the Camera app (or Teams / a browser) and pick "Galaxy S10 Front Camera"; then .\camera-s10.ps1 status'
  }
  'status' { Show-Status }
  'remove' {
    W ([S10SwDev]::Run($false))
    Start-Sleep -Seconds 2
    $pkgs = pnputil /enum-drivers | Out-String
    foreach ($m in [regex]::Matches($pkgs, '(?ms)Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+s10ecamera\.inf')) {
      pnputil /delete-driver $m.Groups[1].Value /uninstall /force 2>&1 | ForEach-Object { W "  $_" }
    }
    Show-Status
  }
}
W "log: $Out"
