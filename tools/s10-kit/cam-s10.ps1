<#
  cam-s10.ps1 (v1.1) - Galaxy S10: front camera bring-up, step by step (S10eCam.sys; notes/s10-camera.md).
  Run ON THE PHONE, PowerShell as Administrator, in the kit folder (with out\S10eCam\ from build-s10.cmd v1.17+):
      .\cam-s10.ps1 install     copy S10eCam.sys to C:\S10Prov\cam and register it (demand start: it only runs when
                                this script asks; nothing happens at boot)
      .\cam-s10.ps1 1           stage 1: READ the camera's clocks, pins, both I2C controllers and the S2MPB02's camera
                                rails. The camera stays OFF. (Sets up the S2MPB02's own I2C bus to read it.)
      .\cam-s10.ps1 2           stage 2: stage 1, then power the front camera (S5K3J1) up exactly as Android does, read
                                its ID, and power it down again (always - also after an error). ~0.1 s.
      .\cam-s10.ps1 3           stage 3: READ the camera's receive path power state (PMU power domains, CMU_TOP). Writes
                                nothing, does not touch the camera or its rails. No risk.
      .\cam-s10.ps1 3deep       stage 3 + READ the blocks inside the ISPPRE domain (CSIS2, its DMA, SysMMU, clocks) -
                                only if '3' said the domain is ON. Writes nothing. Small risk: if the secure firmware
                                still guards that domain, the phone bugchecks and reboots (nothing is written anywhere).
      .\cam-s10.ps1 remove      unregister the driver
  Writes C:\S10Prov\cam-s10-<action>.txt (e.g. cam-s10-3.txt) - send it back. Touches nothing of touch / USB /
  display / storage. Run the stages in order and send each log before the next.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'remove', '1', '2', '3', '3deep')][string]$Action = '1')
$ErrorActionPreference = 'Continue'
$Out = "C:\S10Prov\cam-s10-$Action.txt"
$Dir = 'C:\S10Prov\cam'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
"cam-s10.ps1 v1.1  $Action  $(Get-Date -Format s)" | Set-Content $Out -Encoding ascii
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }

switch ($Action) {
  'install' {
    $src = Join-Path $PSScriptRoot 'out\S10eCam\S10eCam.sys'
    if (-not (Test-Path $src)) { W "no $src - build with build-s10.cmd (EWDK) and copy out\ here"; exit 1 }
    sc.exe stop S10eCam 2>&1 | Out-Null
    New-Item -ItemType Directory -Force $Dir | Out-Null
    Copy-Item $src $Dir -Force
    sc.exe query S10eCam 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { sc.exe create S10eCam type= kernel start= demand binPath= "$Dir\S10eCam.sys" 2>&1 | ForEach-Object { W "  $_" } }
    else { sc.exe config S10eCam binPath= "$Dir\S10eCam.sys" start= demand 2>&1 | ForEach-Object { W "  $_" } }
    W ("installed: {0} ({1})" -f "$Dir\S10eCam.sys", (Get-FileHash "$Dir\S10eCam.sys" -Algorithm SHA256).Hash.Substring(0, 8))
    W 'next: .\cam-s10.ps1 1'
    exit 0
  }
  'remove' {
    sc.exe stop S10eCam 2>&1 | Out-Null
    sc.exe delete S10eCam 2>&1 | ForEach-Object { W "  $_" }
    exit 0
  }
}

$stage = [int]$Action.Substring(0, 1)
$flags = $(if ($Action -eq '3deep') { 1 } else { 0 })   # S10ECAM_FLAG_DEEP
sc.exe start S10eCam 2>&1 | Out-Null
if (-not ('S10Cam' -as [type])) {   # (a v1.0 type in the same session has the old Run signature: use a new window)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class S10Cam {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(IntPtr h, uint code, byte[] i, int il, byte[] o, int ol, out int got, IntPtr ov);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  const uint IOCTL = (0x8A54u << 16) | (0x801u << 2);   // CTL_CODE(0x8A54, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
  public static string Run(int stage, int flags) {
    IntPtr h = CreateFileW(@"\\.\S10eCam", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (h == new IntPtr(-1)) return "open \\\\.\\S10eCam failed: " + Marshal.GetLastWin32Error() + " (driver not started? '.\\cam-s10.ps1 install')";
    byte[] inb = new byte[8]; BitConverter.GetBytes(stage).CopyTo(inb, 0); BitConverter.GetBytes(flags).CopyTo(inb, 4);
    byte[] outb = new byte[16384]; int got;
    bool ok = DeviceIoControl(h, IOCTL, inb, 8, outb, outb.Length, out got, IntPtr.Zero);
    int err = Marshal.GetLastWin32Error(); CloseHandle(h);
    if (!ok) return "ioctl failed: " + err;
    int n = Array.IndexOf(outb, (byte)0); if (n < 0) n = got;
    return Encoding.ASCII.GetString(outb, 0, n);
  }
}
'@
}
$svc = (sc.exe query S10eCam | Out-String)
W ("driver: " + $(if ($svc -match 'RUNNING') { 'running' } else { "NOT running:`n$svc" }))
$rep = [S10Cam]::Run($stage, $flags)
$rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
sc.exe stop S10eCam 2>&1 | Out-Null
W "log: $Out"
