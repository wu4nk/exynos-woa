<#
  cam-s10.ps1 (v1.2) - Galaxy S10: front camera bring-up, step by step (S10eCam.sys; notes/s10-camera.md).
  Run ON THE PHONE, PowerShell as Administrator, in the kit folder (with out\S10eCam\ from build-s10.cmd v1.17+):
      .\cam-s10.ps1 install     copy S10eCam.sys to C:\S10Prov\cam and register it (demand start: it only runs when
                                this script asks; nothing happens at boot)
      .\cam-s10.ps1 1           stage 1: READ the camera's clocks, pins, both I2C controllers and the S2MPB02's camera
                                rails. The camera stays OFF. (Sets up the S2MPB02's own I2C bus to read it.)
      .\cam-s10.ps1 2           stage 2: stage 1, then power the front camera (S5K3J1) up exactly as Android does, read
                                its ID, and power it down again (always - also after an error). ~0.1 s.
      .\cam-s10.ps1 3           stage 3: READ the camera's receive path power state (PMU power domains, CMU_TOP). Writes
                                nothing, does not touch the camera or its rails. No risk.
      .\cam-s10.ps1 3step       the reads INSIDE the ISPPRE domain, ONE small step per driver call (8 steps, safest
                                first). Before each step the log line "step N ... calling" goes to disk write-through,
                                so if the phone blue-screens, the log names the step that did it. Writes nothing.
                                Only after '3' said the domain is ON. (v1.1's '3deep' read it all at once and
                                blue-screened: gone.)
      .\cam-s10.ps1 3step N     the same, starting at step N (after a blue screen at step N-1)
      .\cam-s10.ps1 remove      unregister the driver
  Writes C:\S10Prov\cam-s10-<action>.txt (e.g. cam-s10-3.txt) - send it back. Touches nothing of touch / USB /
  display / storage. Run the stages in order and send each log before the next.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'remove', '1', '2', '3', '3step')][string]$Action = '1',
      [Parameter(Position = 1)][ValidateRange(1, 8)][int]$From = 1)
$ErrorActionPreference = 'Continue'
$Out = "C:\S10Prov\cam-s10-$Action.txt"
$Dir = 'C:\S10Prov\cam'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
# v1.2: every log line goes to disk WRITE-THROUGH + flushed (FlushFileBuffers): 0.3's blue screen lost the whole
# 3deep log from the file cache. '3step' appends (a run after a blue screen keeps the evidence of the one before).
if ($Action -ne '3step') { Remove-Item $Out -Force -ErrorAction SilentlyContinue }
function W($m) {
  Write-Host $m
  $b = [Text.Encoding]::ASCII.GetBytes("$m`r`n")
  $fs = New-Object IO.FileStream($Out, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read, 4096, [IO.FileOptions]::WriteThrough)
  try { $fs.Write($b, 0, $b.Length); $fs.Flush($true) } finally { $fs.Dispose() }
}
W "cam-s10.ps1 v1.2  $Action$(if ($Action -eq '3step') { " from $From" })  $(Get-Date -Format s)"

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
sc.exe start S10eCam 2>&1 | Out-Null
if (-not ('S10Cam' -as [type])) {   # (a v1.0 type in the same window has Run(int) only: open a NEW window)
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
if (-not ([S10Cam].GetMethod('Run').GetParameters().Count -eq 2)) {
  W 'this PowerShell window still has cam-s10 v1.0 loaded (Run with 1 argument): close it, open a NEW one, run again'
  sc.exe stop S10eCam 2>&1 | Out-Null
  exit 1
}
if ($Action -ne '3step') {
  $rep = [S10Cam]::Run($stage, 0)
  $rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
} else {
  $names = @('', 'CMU_ISPPRE mux/divider', 'CMU_ISPPRE clock gates', 'Q-channels CSIS2/DMA', 'Q-channels LHM/SYSREG/SYSMMU',
             'CSIS2 registers', 'CSIS DMA registers', 'SYSREG_ISPPRE', 'SysMMU')
  foreach ($n in $From..8) {
    W "step $n ($($names[$n])): calling the driver ..."
    $rep = [S10Cam]::Run(3, 1 -bor ($n -shl 8))   # S10ECAM_FLAG_DEEP | step << 8
    $lines = @($rep -split "`r?`n" | Where-Object { $_ })
    $start = 0
    if ($n -ne $From) { for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -like '== deep step*') { $start = $i; break } } }
    for ($i = $start; $i -lt $lines.Count; $i++) { W $lines[$i] }
    if (-not ($rep -match "step $n done")) { W "step $n did not complete - stopping here"; break }
    Start-Sleep -Milliseconds 300
  }
}
sc.exe stop S10eCam 2>&1 | Out-Null
W "log: $Out"
