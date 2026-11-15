<#
  cam-s10.ps1 (v1.3.2) - Galaxy S10: front camera bring-up, step by step (S10eCam.sys; notes/s10-camera.md).
  Run ON THE PHONE, PowerShell as Administrator, in the kit folder (with out\S10eCam\ from build-s10.cmd v1.17+):
      .\cam-s10.ps1 install     copy S10eCam.sys to C:\S10Prov\cam and register it (demand start: it only runs when
                                this script asks; nothing happens at boot)
      .\cam-s10.ps1 1           stage 1: READ the camera's clocks, pins, both I2C controllers and the S2MPB02's camera
                                rails. The camera stays OFF. (Sets up the S2MPB02's own I2C bus to read it.)
      .\cam-s10.ps1 2           stage 2: stage 1, then power the front camera (S5K3J1) up exactly as Android does, read
                                its ID, and power it down again (always - also after an error). ~0.1 s.
      .\cam-s10.ps1 3           stage 3: READ the camera's receive path power state (PMU power domains, CMU_TOP). Writes
                                nothing, does not touch the camera or its rails. No risk.
      .\cam-s10.ps1 3step       the reads INSIDE the ISPPRE domain, ONE small step per driver call (7 steps, safest
                                first). Before each step the log line "step N ... calling" goes to disk write-through,
                                so if the phone blue-screens, the log names the step that did it. Writes nothing.
      .\cam-s10.ps1 3step N     the same, starting at step N (after a blue screen at step N-1)
      .\cam-s10.ps1 shot        stage 4 ("3b"): ONE PICTURE from the front camera. The camera is powered up, streams
                                1824 x 1368 for a few frames into memory, and everything is stopped and powered down
                                again. Saves C:\S10Prov\cam-shot.raw (the raw frame) and C:\S10Prov\cam-shot.bmp (a
                                quick 912 x 684 colour preview). ~2 s.
      .\cam-s10.ps1 shot bars   the same with the sensor's own colour-bar test pattern instead of the scene (the
                                first thing to try: bars that look right = the whole receive path is right)
         extra switches for 'shot': -Coarse N (exposure lines, 0x0202) -AGain N (0x0204, 32 = x1) -IspDiv N (the ISPPRE
         bus = 400 MHz / N, default 1; v1.3's 100 MHz made the receiver overflow) -Frames N (frames to wait for, default 4) -Phase N (stop after
         phase N and tear down: 1 sensor set up, 2 + D-PHY, 3 + CSIS2, 4 + DMA armed - only to find a blue screen's step)
      .\cam-s10.ps1 remove      unregister the driver
  Writes C:\S10Prov\cam-s10-<action>.txt (e.g. cam-s10-shot.txt) - send it back, with cam-shot.bmp for 'shot'.
  Touches nothing of touch / USB / display / storage. Run the stages in order and send each log before the next.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'remove', '1', '2', '3', '3step', 'shot')][string]$Action = '1',
      [Parameter(Position = 1)][string]$Opt = '',
      [int]$Coarse = 0, [int]$AGain = 0, [int]$IspDiv = 0, [int]$Frames = 0, [ValidateRange(0, 4)][int]$Phase = 0)
$ErrorActionPreference = 'Continue'
$Out = "C:\S10Prov\cam-s10-$Action.txt"
$Dir = 'C:\S10Prov\cam'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$From = 1
if ($Action -eq '3step' -and $Opt) {
  $From = [int]$Opt
  if ($From -lt 1 -or $From -gt 7) { Write-Host '3step N: N is 1..7'; exit 1 }
}
if ($Action -eq 'shot' -and $Opt -and $Opt -ne 'bars') { Write-Host "shot: the only option is 'bars'"; exit 1 }
# v1.2: every log line goes to disk WRITE-THROUGH + flushed (FlushFileBuffers): 0.3's blue screen lost the whole
# 3deep log from the file cache. '3step' appends (a run after a blue screen keeps the evidence of the one before).
if ($Action -ne '3step') { Remove-Item $Out -Force -ErrorAction SilentlyContinue }
function W($m) {
  Write-Host $m
  $b = [Text.Encoding]::ASCII.GetBytes("$m`r`n")
  $fs = New-Object IO.FileStream($Out, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read, 4096, [IO.FileOptions]::WriteThrough)
  try { $fs.Write($b, 0, $b.Length); $fs.Flush($true) } finally { $fs.Dispose() }
}
W "cam-s10.ps1 v1.3.2  $Action $Opt$(if ($Action -eq '3step') { " from $From" })  $(Get-Date -Format s)"

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

$stage = if ($Action -eq 'shot') { 4 } else { [int]$Action.Substring(0, 1) }
sc.exe start S10eCam 2>&1 | Out-Null
# v1.3: a new type name (S10Cam13): a window that already loaded v1.0-v1.2's S10Cam keeps that old type.
if (-not ('S10Cam13' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
public static class S10Cam13 {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(IntPtr h, uint code, byte[] i, int il, byte[] o, int ol, out int got, IntPtr ov);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  const uint IOCTL_STAGE = (0x8A54u << 16) | (0x801u << 2);        // CTL_CODE(0x8A54, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
  const uint IOCTL_SHOT = (0x8A54u << 16) | (0x802u << 2) | 2u;    // CTL_CODE(0x8A54, 0x802, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
  public const int W = 1824, H = 1368, STRIDE = W * 2, BYTES = STRIDE * H, HDR = 16384;
  static IntPtr Open() { return CreateFileW(@"\\.\S10eCam", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero); }
  static string Text(byte[] b, int max) { int n = Array.IndexOf(b, (byte)0); if (n < 0 || n > max) n = max; return Encoding.ASCII.GetString(b, 0, n); }
  public static string Run(int stage, int flags) {
    IntPtr h = Open();
    if (h == new IntPtr(-1)) return "open \\\\.\\S10eCam failed: " + Marshal.GetLastWin32Error() + " (driver not started? '.\\cam-s10.ps1 install')";
    byte[] inb = new byte[8]; BitConverter.GetBytes(stage).CopyTo(inb, 0); BitConverter.GetBytes(flags).CopyTo(inb, 4);
    byte[] outb = new byte[16384]; int got;
    bool ok = DeviceIoControl(h, IOCTL_STAGE, inb, 8, outb, outb.Length, out got, IntPtr.Zero);
    int err = Marshal.GetLastWin32Error(); CloseHandle(h);
    if (!ok) return "ioctl failed: " + err;
    return Text(outb, got);
  }
  // S10ECAM_SHOT_IN {Flags, Coarse, AGain, IspDiv, Frames}; output = HDR bytes of report, then the frame
  public static string Shot(int flags, int coarse, int again, int ispdiv, int frames, string raw, string bmp) {
    IntPtr h = Open();
    if (h == new IntPtr(-1)) return "open \\\\.\\S10eCam failed: " + Marshal.GetLastWin32Error() + " (driver not started? '.\\cam-s10.ps1 install')";
    byte[] inb = new byte[20];
    BitConverter.GetBytes(flags).CopyTo(inb, 0); BitConverter.GetBytes(coarse).CopyTo(inb, 4); BitConverter.GetBytes(again).CopyTo(inb, 8);
    BitConverter.GetBytes(ispdiv).CopyTo(inb, 12); BitConverter.GetBytes(frames).CopyTo(inb, 16);
    byte[] outb = new byte[HDR + BYTES]; int got;
    bool ok = DeviceIoControl(h, IOCTL_SHOT, inb, inb.Length, outb, outb.Length, out got, IntPtr.Zero);
    int err = Marshal.GetLastWin32Error(); CloseHandle(h);
    if (!ok) return "ioctl failed: " + err + (err == 1 ? " (ERROR_INVALID_FUNCTION: this S10eCam.sys is older than 0.5 - rebuild + install)" : "");
    string rep = Text(outb, HDR);
    if (got >= HDR + BYTES) {
      using (FileStream f = new FileStream(raw, FileMode.Create)) f.Write(outb, HDR, BYTES);
      rep += "saved " + raw + " (" + BYTES + " bytes)\r\n" + Preview(outb, HDR, bmp);
    } else rep += "no frame came back (" + got + " bytes)\r\n";
    return rep;
  }
  // 2x2 binning of GRBG (row 0: G R, row 1: B G) -> 912 x 684 24-bit BMP; black level 64 subtracted (the sensor's
  // pedestal: a dark scene reads 64 +- 2, 2026-11-16), white point = the 99.5th percentile of the brightest channel
  // but at least 32 above black (v1.3.1 stretched a black frame's noise to white), gamma 1/2.2. 10-bit values.
  static string Preview(byte[] b, int off, string path) {
    int ow = W / 2, oh = H / 2;
    int[] rgb = new int[ow * oh * 3];
    int[] hist = new int[1024];
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
      int o0 = off + (2 * y) * STRIDE + 4 * x, o1 = o0 + STRIDE;
      int g1 = (b[o0] | b[o0 + 1] << 8) & 0x3FF, r = (b[o0 + 2] | b[o0 + 3] << 8) & 0x3FF;
      int bl = (b[o1] | b[o1 + 1] << 8) & 0x3FF, g2 = (b[o1 + 2] | b[o1 + 3] << 8) & 0x3FF;
      int i = (y * ow + x) * 3; rgb[i] = r; rgb[i + 1] = (g1 + g2) / 2; rgb[i + 2] = bl;
      hist[Math.Max(r, Math.Max(rgb[i + 1], bl))]++;
    }
    const int BLACK = 64;
    int total = ow * oh, acc = 0, white = 1023;
    for (int v = 0; v < 1024; v++) { acc += hist[v]; if (acc >= total * 0.995) { white = Math.Max(v, BLACK + 32); break; } }
    byte[] lut = new byte[1024];
    for (int v = 0; v < 1024; v++) lut[v] = (byte)Math.Min(255.0, Math.Round(255.0 * Math.Pow(Math.Max(0.0, Math.Min(1.0, (double)(v - BLACK) / (white - BLACK))), 1 / 2.2)));
    int rowBytes = ow * 3, pad = (4 - rowBytes % 4) % 4, img = (rowBytes + pad) * oh;
    using (BinaryWriter w = new BinaryWriter(new FileStream(path, FileMode.Create))) {
      w.Write((byte)'B'); w.Write((byte)'M'); w.Write(54 + img); w.Write(0); w.Write(54);
      w.Write(40); w.Write(ow); w.Write(oh); w.Write((short)1); w.Write((short)24); w.Write(0); w.Write(img);
      w.Write(2835); w.Write(2835); w.Write(0); w.Write(0);
      for (int y = oh - 1; y >= 0; y--) {
        for (int x = 0; x < ow; x++) { int i = (y * ow + x) * 3; w.Write(lut[rgb[i + 2]]); w.Write(lut[rgb[i + 1]]); w.Write(lut[rgb[i]]); }
        for (int p = 0; p < pad; p++) w.Write((byte)0);
      }
    }
    return "saved " + path + " (" + ow + " x " + oh + " preview, white point " + white + " / 1023)\r\n";
  }
}
'@
}
$svc = (sc.exe query S10eCam | Out-String)
W ("driver: " + $(if ($svc -match 'RUNNING') { 'running' } else { "NOT running:`n$svc" }))
if ($Action -eq 'shot') {
  $rep = [S10Cam13]::Shot($(if ($Opt -eq 'bars') { 1 } else { 0 }) -bor ($Phase -shl 8), $Coarse, $AGain, $IspDiv, $Frames,
                          'C:\S10Prov\cam-shot.raw', 'C:\S10Prov\cam-shot.bmp')
  $rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
  if (Test-Path 'C:\S10Prov\cam-shot.bmp') { W 'open C:\S10Prov\cam-shot.bmp to look at it' }
} elseif ($Action -ne '3step') {
  $rep = [S10Cam13]::Run($stage, 0)
  $rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
} else {
  $names = @('', 'CMU_ISPPRE mux/divider', 'CMU_ISPPRE clock gates', 'Q-channels CSIS2/DMA', 'Q-channels LHM/SYSREG/SYSMMU',
             'CSIS2 registers', 'CSIS DMA registers', 'SYSREG_ISPPRE')   # v1.2.1: step 8 (SysMMU) aborts - removed
  foreach ($n in $From..7) {
    W "step $n ($($names[$n])): calling the driver ..."
    $rep = [S10Cam13]::Run(3, 1 -bor ($n -shl 8))   # S10ECAM_FLAG_DEEP | step << 8
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
