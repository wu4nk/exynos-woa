<#
  cam-s10.ps1 (v1.7) - Galaxy S10: front camera bring-up, step by step (S10eCam.sys; notes/s10-camera.md).
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
      .\cam-s10.ps1 isp         (v1.7, S10eCam 0.11; 0.12 also prints pd-isplp / pd-isphq) READ the ISP's 3AA0 ID registers (+0xF0/+0xF4), only if its clock gate
                                is on - is the Samsung ISP block reachable from Windows? Android's values: 0x07010000 /
                                0x0000CC22. A blue screen here (bugcheck 0x124) = the block refuses Windows' reads (as
                                the ISPPRE SysMMU did): send WINPRAM / the log. Writes nothing.
      .\cam-s10.ps1 shot        stage 4 ("3b"): ONE PICTURE from the front camera. The camera is powered up, streams
                                1824 x 1368 for a few frames into memory, and everything is stopped and powered down
                                again. Saves C:\S10Prov\cam-shot.raw (the raw frame) and C:\S10Prov\cam-shot.bmp (a
                                quick 912 x 684 colour preview). ~2 s.
      .\cam-s10.ps1 shot bars   the same with the sensor's own colour-bar test pattern instead of the scene (the
                                first thing to try: bars that look right = the whole receive path is right)
         extra switches for 'shot': -Coarse N (exposure lines, 0x0202) -AGain N (0x0204, 32 = x1) -IspDiv N (the ISPPRE
         bus = 400 MHz / N, default 1; v1.3's 100 MHz made the receiver overflow) -Frames N (frames to wait for, default 4) -Phase N (stop after
         phase N and tear down: 1 sensor set up, 2 + D-PHY, 3 + CSIS2, 4 + DMA armed - only to find a blue screen's step)
      .\cam-s10.ps1 stream      (S10eCam 0.6) the camera RUNS for a few seconds with auto exposure: frames are pulled,
                                exposure + gain are set from their brightness until it settles, then the last frame is
                                saved with white balance and full-resolution colour: C:\S10Prov\cam-stream.bmp
                                (1824 x 1368) + cam-stream.raw. The camera is stopped and powered down at the end (also
                                if the script is interrupted: closing the handle stops it).
      .\cam-s10.ps1 stream bars  the same on the test pattern (checks the streaming ring: every frame must be bars)
         extra switches for 'stream': -Seconds N (default 4) -Rotate 0|90|180|270 (default 0 = as the sensor reads out;
         270 = upright, found 2026-11-16) -Mirror (v1.5: flip the SAVED PICTURE left-right after rotating; whether the
         sensor's readout is mirrored is not verified yet - hold up some text) -Wc (S10eCam 0.7: the frame ring
         write-combined instead of non-cached - compare the "copy ... us" column with and without it)
         -Bin (v1.6, S10eCam 0.9: the sensor's own 912 x 684 mode, 4 x 4 binned, at 30 fps - 1/4 of the data; for
         shot and stream)
      .\cam-s10.ps1 remove      unregister the driver
  Writes C:\S10Prov\cam-s10-<action>.txt (e.g. cam-s10-shot.txt) - send it back, with cam-shot.bmp for 'shot'.
  Touches nothing of touch / USB / display / storage. Run the stages in order and send each log before the next.
#>
param([Parameter(Position = 0)][ValidateSet('install', 'remove', '1', '2', '3', '3step', 'isp', 'shot', 'stream')][string]$Action = '1',
      [Parameter(Position = 1)][string]$Opt = '',
      [int]$Coarse = 0, [int]$AGain = 0, [int]$IspDiv = 0, [int]$Frames = 0, [ValidateRange(0, 4)][int]$Phase = 0,
      [ValidateRange(1, 60)][int]$Seconds = 4, [ValidateSet(0, 90, 180, 270)][int]$Rotate = 0, [switch]$Mirror,
      [switch]$Wc, [switch]$Bin)
$ErrorActionPreference = 'Continue'
$Out = "C:\S10Prov\cam-s10-$Action.txt"
$Dir = 'C:\S10Prov\cam'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$From = 1
if ($Action -eq '3step' -and $Opt) {
  $From = [int]$Opt
  if ($From -lt 1 -or $From -gt 7) { Write-Host '3step N: N is 1..7'; exit 1 }
}
if ($Action -in 'shot', 'stream' -and $Opt -and $Opt -ne 'bars') { Write-Host "${Action}: the only option is 'bars'"; exit 1 }
# v1.2: every log line goes to disk WRITE-THROUGH + flushed (FlushFileBuffers): 0.3's blue screen lost the whole
# 3deep log from the file cache. '3step' appends (a run after a blue screen keeps the evidence of the one before).
if ($Action -ne '3step') { Remove-Item $Out -Force -ErrorAction SilentlyContinue }
function W($m) {
  Write-Host $m
  $b = [Text.Encoding]::ASCII.GetBytes("$m`r`n")
  $fs = New-Object IO.FileStream($Out, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read, 4096, [IO.FileOptions]::WriteThrough)
  try { $fs.Write($b, 0, $b.Length); $fs.Flush($true) } finally { $fs.Dispose() }
}
W "cam-s10.ps1 v1.7  $Action $Opt$(if ($Action -eq '3step') { " from $From" })  $(Get-Date -Format s)"

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

$stage = if ($Action -in 'shot', 'stream') { 4 } elseif ($Action -eq 'isp') { 3 } else { [int]$Action.Substring(0, 1) }
sc.exe start S10eCam 2>&1 | Out-Null
# v1.3+: a new type name per version (S10Cam16): a window that loaded an older one keeps that old type.
if (-not ('S10Cam16' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
public static class S10Cam16 {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(IntPtr h, uint code, byte[] i, int il, byte[] o, int ol, out int got, IntPtr ov);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  const uint IOCTL_STAGE = (0x8A54u << 16) | (0x801u << 2);        // CTL_CODE(0x8A54, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
  const uint IOCTL_SHOT = (0x8A54u << 16) | (0x802u << 2) | 2u;    // CTL_CODE(0x8A54, 0x802, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
  // v1.6: the frame size follows the mode (S10eCam 0.9 -Bin = 912 x 684); buffers are sized for the big one
  public static int W = 1824, H = 1368;
  public static int STRIDE { get { return W * 2; } }
  public static int BYTES { get { return STRIDE * H; } }
  public const int MAXBYTES = 1824 * 2 * 1368, HDR = 16384;
  public static void SetMode(bool bin) { W = bin ? 912 : 1824; H = bin ? 684 : 1368; }
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
    SetMode((flags & 4) != 0);
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
  // ---- streaming (S10eCam 0.6): ONE handle for the whole run - closing it stops the camera (IRP_MJ_CLEANUP)
  const uint IOCTL_START = (0x8A54u << 16) | (0x803u << 2);
  const uint IOCTL_FRAME = (0x8A54u << 16) | (0x804u << 2) | 2u;
  const uint IOCTL_EXPO  = (0x8A54u << 16) | (0x805u << 2);
  const uint IOCTL_STOP  = (0x8A54u << 16) | (0x806u << 2);
  public const int FHDR = 64;
  static IntPtr sh = new IntPtr(-1);
  public static string StreamStart(int flags, int coarse, int again, int ispdiv) {
    sh = Open();
    if (sh == new IntPtr(-1)) return "open \\\\.\\S10eCam failed: " + Marshal.GetLastWin32Error();
    byte[] inb = new byte[20];
    BitConverter.GetBytes(flags).CopyTo(inb, 0); BitConverter.GetBytes(coarse).CopyTo(inb, 4); BitConverter.GetBytes(again).CopyTo(inb, 8);
    BitConverter.GetBytes(ispdiv).CopyTo(inb, 12);
    byte[] outb = new byte[16384]; int got;
    if (!DeviceIoControl(sh, IOCTL_START, inb, inb.Length, outb, outb.Length, out got, IntPtr.Zero))
      return "ioctl failed: " + Marshal.GetLastWin32Error() + (Marshal.GetLastWin32Error() == 1 ? " (this S10eCam.sys is older than 0.6 - rebuild + install)" : "");
    return Text(outb, got);
  }
  public static string StreamStop() {
    if (sh == new IntPtr(-1)) return "";
    byte[] outb = new byte[16384]; int got;
    string r = DeviceIoControl(sh, IOCTL_STOP, null, 0, outb, outb.Length, out got, IntPtr.Zero) ? Text(outb, got) : "stop ioctl failed: " + Marshal.GetLastWin32Error();
    CloseHandle(sh); sh = new IntPtr(-1);
    return r;
  }
  public static bool Exposure(int coarse, int again) {
    byte[] inb = new byte[8]; BitConverter.GetBytes(coarse).CopyTo(inb, 0); BitConverter.GetBytes(again).CopyTo(inb, 4);
    int got; return DeviceIoControl(sh, IOCTL_EXPO, inb, 8, null, 0, out got, IntPtr.Zero);
  }
  // fills buf (FHDR + BYTES); returns the header fields, or null on an ioctl failure
  public static uint[] Frame(byte[] buf) {
    int got;
    if (!DeviceIoControl(sh, IOCTL_FRAME, null, 0, buf, buf.Length, out got, IntPtr.Zero)) return null;
    uint[] h = new uint[16];
    for (int i = 0; i < 16; i++) h[i] = BitConverter.ToUInt32(buf, 4 * i);
    if (h[15] != 0) { W = (int)(h[15] & 0xFFFF); H = (int)(h[15] >> 16); }   // S10eCam 0.9+: WxH
    return h;   // Magic Seq Ring AgeMs Coarse AGain AGainMin AGainMax CoarseMax Rearms Stalls IntSrc0 DmaIntSrc CopyUs Torn WxH
  }
  // black-subtracted channel means over a sparse grid + the share of clipped pixels: {G, R, B, clipped %}
  public static double[] Stats(byte[] b, int off) {
    double g = 0, r = 0, bl = 0; int n = 0, clip = 0;
    for (int y = 0; y < H; y += 16) for (int x = 0; x < W; x += 16) {
      int o0 = off + y * STRIDE + 2 * x, o1 = o0 + STRIDE;
      int g1 = (b[o0] | b[o0 + 1] << 8) & 0x3FF, rr = (b[o0 + 2] | b[o0 + 3] << 8) & 0x3FF;
      int bb = (b[o1] | b[o1 + 1] << 8) & 0x3FF, g2 = (b[o1 + 2] | b[o1 + 3] << 8) & 0x3FF;
      g += Math.Max(0, (g1 + g2) / 2 - 64); r += Math.Max(0, rr - 64); bl += Math.Max(0, bb - 64); n++;
      if (g1 > 1000 || rr > 1000 || bb > 1000) clip++;
    }
    return new double[] { g / n, r / n, bl / n, 100.0 * clip / n };
  }
  // full-resolution bilinear demosaic of GRBG (row 0: G R, row 1: B G), black 64, white balance gains (R, B) on G,
  // white point = p99.5 of G, gamma 1/2.2, rotation 0/90/180/270 clockwise -> 24-bit BMP
  public static string Develop(byte[] b, int off, double wbR, double wbB, int rot, bool mirror, string path) {
    int[] v = new int[W * H];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { int o = off + y * STRIDE + 2 * x; v[y * W + x] = Math.Max(0, ((b[o] | b[o + 1] << 8) & 0x3FF) - 64); }
    Func<int, int, int> P = (x, y) => v[Math.Min(H - 1, Math.Max(0, y)) * W + Math.Min(W - 1, Math.Max(0, x))];
    float[] R = new float[W * H], G = new float[W * H], B = new float[W * H];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
      int i = y * W + x; bool ey = (y & 1) == 0, ex = (x & 1) == 0;
      float c = P(x, y), cross = (P(x - 1, y) + P(x + 1, y) + P(x, y - 1) + P(x, y + 1)) / 4f, diag = (P(x - 1, y - 1) + P(x + 1, y - 1) + P(x - 1, y + 1) + P(x + 1, y + 1)) / 4f;
      float hor = (P(x - 1, y) + P(x + 1, y)) / 2f, ver = (P(x, y - 1) + P(x, y + 1)) / 2f;
      if (ey && ex) { G[i] = c; R[i] = hor; B[i] = ver; }          // G on an R row
      else if (ey) { R[i] = c; G[i] = cross; B[i] = diag; }        // R
      else if (ex) { B[i] = c; G[i] = cross; R[i] = diag; }        // B
      else { G[i] = c; B[i] = hor; R[i] = ver; }                   // G on a B row
    }
    float[] gs = new float[(W / 4) * (H / 4)]; int k = 0;
    for (int y = 0; y < H; y += 4) for (int x = 0; x < W; x += 4) if (k < gs.Length) gs[k++] = G[y * W + x];
    Array.Sort(gs); float white = Math.Max(32f, gs[(int)(gs.Length * 0.995)]);
    byte[] lut = new byte[4096];
    for (int i = 0; i < 4096; i++) lut[i] = (byte)Math.Min(255.0, Math.Round(255.0 * Math.Pow(i / 4095.0, 1 / 2.2)));
    Func<float, byte> T = f => lut[(int)Math.Min(4095f, Math.Max(0f, f / white * 4095f))];
    int ow = (rot == 90 || rot == 270) ? H : W, oh = (rot == 90 || rot == 270) ? W : H;
    int rowBytes = ow * 3, pad = (4 - rowBytes % 4) % 4, img = (rowBytes + pad) * oh;
    using (BinaryWriter w = new BinaryWriter(new FileStream(path, FileMode.Create))) {
      w.Write((byte)'B'); w.Write((byte)'M'); w.Write(54 + img); w.Write(0); w.Write(54);
      w.Write(40); w.Write(ow); w.Write(oh); w.Write((short)1); w.Write((short)24); w.Write(0); w.Write(img);
      w.Write(2835); w.Write(2835); w.Write(0); w.Write(0);
      byte[] line = new byte[rowBytes + pad];
      for (int oy = oh - 1; oy >= 0; oy--) {
        for (int ox0 = 0; ox0 < ow; ox0++) {
          int ox = mirror ? ow - 1 - ox0 : ox0;   // v1.5: mirror = the picture flipped left-right after rotating
          int sx, sy;   // output (ox, oy) <- source (sx, sy), clockwise rotation
          if (rot == 90) { sx = oy; sy = H - 1 - ox; } else if (rot == 180) { sx = W - 1 - ox; sy = H - 1 - oy; }
          else if (rot == 270) { sx = W - 1 - oy; sy = ox; } else { sx = ox; sy = oy; }
          int i = sy * W + sx;
          line[3 * ox0] = T(B[i] * (float)wbB); line[3 * ox0 + 1] = T(G[i]); line[3 * ox0 + 2] = T(R[i] * (float)wbR);
        }
        w.Write(line);
      }
    }
    return "saved " + path + " (" + ow + " x " + oh + ", rotate " + rot + (mirror ? " + mirror" : "") + ", WB R x" + wbR.ToString("0.00") + " B x" + wbB.ToString("0.00") + ", white " + white.ToString("0") + ")\r\n";
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
  $rep = [S10Cam16]::Shot($(if ($Opt -eq 'bars') { 1 } else { 0 }) -bor $(if ($Wc) { 2 } else { 0 }) -bor $(if ($Bin) { 4 } else { 0 }) -bor ($Phase -shl 8), $Coarse, $AGain, $IspDiv, $Frames,
                          'C:\S10Prov\cam-shot.raw', 'C:\S10Prov\cam-shot.bmp')
  $rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
  if (Test-Path 'C:\S10Prov\cam-shot.bmp') { W 'open C:\S10Prov\cam-shot.bmp to look at it' }
} elseif ($Action -eq 'stream') {
  [S10Cam16]::SetMode($Bin.IsPresent)
  $rep = [S10Cam16]::StreamStart($(if ($Opt -eq 'bars') { 1 } else { 0 }) -bor $(if ($Wc) { 2 } else { 0 }) -bor $(if ($Bin) { 4 } else { 0 }), $Coarse, $AGain, $IspDiv)
  $rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
  $buf = New-Object byte[] ([S10Cam16]::FHDR + [S10Cam16]::MAXBYTES)
  $last = $null; $lastSeq = 0; $applied = 0; $settled = $false
  $target = 110.0                                   # black-subtracted green mean (of 959) ~ mid-grey after gamma
  try {
    if ($rep -notmatch 'stream start: 0x00000000') { throw 'the stream did not start' }
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
      Start-Sleep -Milliseconds 120
      $h = [S10Cam16]::Frame($buf)
      if ($null -eq $h) { W 'frame ioctl failed'; break }
      if ($h[1] -eq 0) { W ('  no frame yet  (re-arms {0}, stalls {1}, INT_SRC0 {2:X8}, DMA {3:X8})' -f $h[9], $h[10], $h[11], $h[12]); continue }
      $st = [S10Cam16]::Stats($buf, [S10Cam16]::FHDR)
      W ('  seq {0,4} ring {1} age {2,3} ms copy {3,5} us torn {4}  coarse {5,4} again 0x{6:X3} ({7:X}-{8:X})  G {9,6:0.0} R {10,6:0.0} B {11,6:0.0} clip {12,4:0.0}%  INT {13:X8} DMA {14:X8} rearm {15} stall {16}' -f `
        $h[1], $h[2], $h[3], $h[13], $h[14], $h[4], $h[5], $h[6], $h[7], $st[0], $st[1], $st[2], $st[3], $h[11], $h[12], $h[9], $h[10])
      $last = $st; $lastSeq = $h[1]
      if ($Opt -eq 'bars' -or $h[1] -lt $applied + 3) { continue }   # an exposure applies 1-2 frames after it is written
      # AE: exposure product E = lines x gain/32; move it toward the target (clipping counts as too bright)
      $e = [double]$h[4] * $h[5] / 32.0
      $ratio = $(if ($st[3] -gt 5) { 0.5 } elseif ($st[0] -lt 1) { 4.0 } else { [Math]::Min(4.0, [Math]::Max(0.25, $target / $st[0])) })
      if ([Math]::Abs($ratio - 1.0) -lt 0.12) { if (-not $settled) { W '  AE settled' }; $settled = $true; continue }
      $settled = $false
      $e2 = $e * $ratio
      $c2 = [int][Math]::Max(2, [Math]::Min([double]$h[8], $e2))
      $g2 = [int][Math]::Round([Math]::Max([double]$h[6], [Math]::Min([double]$h[7], $e2 / $c2 * 32.0)))
      if ([S10Cam16]::Exposure($c2, $g2)) { $applied = $h[1] } else { W '  exposure ioctl failed' }
    }
    if ($lastSeq -gt 0) {
      $fs = [IO.File]::Create('C:\S10Prov\cam-stream.raw'); $fs.Write($buf, [S10Cam16]::FHDR, [S10Cam16]::BYTES); $fs.Dispose()
      $wbR = $(if ($Opt -eq 'bars' -or $last[1] -lt 1) { 1.0 } else { [Math]::Min(4.0, [Math]::Max(0.25, $last[0] / $last[1])) })
      $wbB = $(if ($Opt -eq 'bars' -or $last[2] -lt 1) { 1.0 } else { [Math]::Min(4.0, [Math]::Max(0.25, $last[0] / $last[2])) })
      W ([S10Cam16]::Develop($buf, [S10Cam16]::FHDR, $wbR, $wbB, $Rotate, $Mirror.IsPresent, 'C:\S10Prov\cam-stream.bmp')).Trim()
      W 'open C:\S10Prov\cam-stream.bmp to look at it'
    }
  } catch { W "stopped: $_" }
  finally {
    $rep = [S10Cam16]::StreamStop()
    $rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
  }
} elseif ($Action -eq 'isp') {
  # v1.7: deep step 8 alone (S10eCam 0.11) - the 'calling' line is on disk before the read, as for 3step
  W 'step 8 (3AA0 ID registers): calling the driver ...'
  $rep = [S10Cam16]::Run(3, 1 -bor (8 -shl 8))   # S10ECAM_FLAG_DEEP | step 8
  $rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
} elseif ($Action -ne '3step') {
  $rep = [S10Cam16]::Run($stage, 0)
  $rep -split "`r?`n" | ForEach-Object { if ($_) { W $_ } }
} else {
  $names = @('', 'CMU_ISPPRE mux/divider', 'CMU_ISPPRE clock gates', 'Q-channels CSIS2/DMA', 'Q-channels LHM/SYSREG/SYSMMU',
             'CSIS2 registers', 'CSIS DMA registers', 'SYSREG_ISPPRE')   # v1.2.1: step 8 (SysMMU) aborts - removed
  foreach ($n in $From..7) {
    W "step $n ($($names[$n])): calling the driver ..."
    $rep = [S10Cam16]::Run(3, 1 -bor ($n -shl 8))   # S10ECAM_FLAG_DEEP | step << 8
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
