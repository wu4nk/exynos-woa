<#
  camprobe-s10.ps1 (v1.0) - is it the camera or the app? Opens "Galaxy S10 Front Camera" the way apps do
  (Windows.Media.Capture, through the Windows camera frame server) and reads frames for a few seconds, then saves one.
  Run ON THE PHONE in Windows PowerShell (5.1; no admin needed), with every camera app closed:
      .\camprobe-s10.ps1 cpu      frames in system memory (MemoryPreference Cpu): no GPU in the path
      .\camprobe-s10.ps1 gpu      frames as Direct3D surfaces (MemoryPreference Auto - what the Camera app gets)
  -Seconds N (default 5). Writes C:\S10Prov\camprobe-<mode>.txt and C:\S10Prov\camprobe-<mode>.bmp - send them back.
  A DIAGNOSIS tool (notes/s10-camera.md, 2026-11-17): it tells a camera-side failure from a GPU-side one.
#>
param([Parameter(Position = 0)][ValidateSet('cpu', 'gpu')][string]$Mode = 'cpu', [ValidateRange(1, 60)][int]$Seconds = 5)
$ErrorActionPreference = 'Stop'
$Out = "C:\S10Prov\camprobe-$Mode.txt"
$Bmp = "C:\S10Prov\camprobe-$Mode.bmp"
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Remove-Item $Out, $Bmp -Force -ErrorAction SilentlyContinue
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }
W "camprobe-s10.ps1 v1.0  $Mode  $(Get-Date -Format s)  PowerShell $($PSVersionTable.PSVersion)"
if ($PSVersionTable.PSVersion.Major -gt 5) { W 'run it in Windows PowerShell 5.1 (powershell.exe): WinRT types are not reachable from PowerShell 7'; exit 1 }

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.Capture.MediaCapture, Windows.Media.Capture, ContentType = WindowsRuntime]
$null = [Windows.Media.Capture.Frames.MediaFrameSourceGroup, Windows.Media.Capture, ContentType = WindowsRuntime]
$null = [Windows.Media.Capture.Frames.MediaFrameReader, Windows.Media.Capture, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.SoftwareBitmap, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.Buffer, Windows.Storage.Streams, ContentType = WindowsRuntime]

# WinRT async -> .NET Task (the usual PowerShell 5.1 pattern)
$asTaskOp = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
$asTaskAct = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } | Select-Object -First 1
function Await($op, [Type]$t) { $task = $asTaskOp.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }
function AwaitAction($a) { $task = $asTaskAct.Invoke($null, @($a)); $task.Wait(-1) | Out-Null }
function Err($e) { $x = $e.Exception; while ($x.InnerException) { $x = $x.InnerException }; '{0} (HRESULT 0x{1:X8})' -f $x.Message.Trim(), $x.HResult }

$mc = $null; $reader = $null
try {
  $groups = Await ([Windows.Media.Capture.Frames.MediaFrameSourceGroup]::FindAllAsync()) ([System.Collections.Generic.IReadOnlyList[Windows.Media.Capture.Frames.MediaFrameSourceGroup]])
  foreach ($g in $groups) { W ("source group: '{0}'  {1}" -f $g.DisplayName, $g.Id) }
  $grp = $groups | Where-Object { $_.DisplayName -match 'Galaxy S10' } | Select-Object -First 1
  if (-not $grp) { W 'no "Galaxy S10" source group - is the camera installed (camera-s10.ps1 status)?'; exit 1 }

  $set = New-Object Windows.Media.Capture.MediaCaptureInitializationSettings
  $set.SourceGroup = $grp
  $set.SharingMode = [Windows.Media.Capture.MediaCaptureSharingMode]::ExclusiveControl
  $set.StreamingCaptureMode = [Windows.Media.Capture.StreamingCaptureMode]::Video
  $set.MemoryPreference = $(if ($Mode -eq 'cpu') { [Windows.Media.Capture.MediaCaptureMemoryPreference]::Cpu } else { [Windows.Media.Capture.MediaCaptureMemoryPreference]::Auto })
  $mc = New-Object Windows.Media.Capture.MediaCapture
  try { AwaitAction ($mc.InitializeAsync($set)); W 'MediaCapture.InitializeAsync: ok' }
  catch { W ('MediaCapture.InitializeAsync FAILED: ' + (Err $_)); exit 1 }

  $src = $mc.FrameSources.Values | Where-Object { $_.Info.SourceKind -eq [Windows.Media.Capture.Frames.MediaFrameSourceKind]::Color } | Select-Object -First 1
  if (-not $src) { W 'no colour frame source'; exit 1 }
  foreach ($f in $src.SupportedFormats) { W ("  format: {0} {1} x {2} @ {3}/{4}" -f $f.Subtype, $f.VideoFormat.Width, $f.VideoFormat.Height, $f.FrameRate.Numerator, $f.FrameRate.Denominator) }
  $cf = $src.CurrentFormat
  W ("current: {0} {1} x {2}" -f $cf.Subtype, $cf.VideoFormat.Width, $cf.VideoFormat.Height)

  try { $reader = Await ($mc.CreateFrameReaderAsync($src)) ([Windows.Media.Capture.Frames.MediaFrameReader]) }
  catch { W ('CreateFrameReaderAsync FAILED: ' + (Err $_)); exit 1 }
  $st = Await ($reader.StartAsync()) ([Windows.Media.Capture.Frames.MediaFrameReaderStartStatus])
  W "MediaFrameReader.StartAsync: $st"
  if ($st -ne [Windows.Media.Capture.Frames.MediaFrameReaderStartStatus]::Success) { exit 1 }

  $n = 0; $sw = 0; $d3d = 0; $saved = $false; $t0 = Get-Date; $first = $null; $lastTs = $null
  while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $fr = $reader.TryAcquireLatestFrame()
    if ($null -eq $fr) { Start-Sleep -Milliseconds 15; continue }
    if ($fr.SystemRelativeTime -ne $lastTs) {
      $lastTs = $fr.SystemRelativeTime; $n++
      if ($null -eq $first) { $first = ((Get-Date) - $t0).TotalMilliseconds }
      $vf = $fr.VideoMediaFrame
      if ($vf.SoftwareBitmap) { $sw++ }
      if ($vf.Direct3DSurface) { $d3d++ }
      if (-not $saved -and $vf.SoftwareBitmap -and $n -ge 5) {
        $sb = [Windows.Graphics.Imaging.SoftwareBitmap]::Convert($vf.SoftwareBitmap, [Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8)
        $w = $sb.PixelWidth; $h = $sb.PixelHeight
        $buf = New-Object Windows.Storage.Streams.Buffer ([uint32]($w * $h * 4))
        $sb.CopyToBuffer($buf)
        $px = [System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions]::ToArray($buf)
        $img = $w * $h * 4
        $fs = [IO.File]::Create($Bmp); $bw = New-Object IO.BinaryWriter($fs)
        $bw.Write([byte]0x42); $bw.Write([byte]0x4D); $bw.Write([int](54 + $img)); $bw.Write([int]0); $bw.Write([int]54)
        $bw.Write([int]40); $bw.Write([int]$w); $bw.Write([int](-$h)); $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
        $bw.Write([int]$img); $bw.Write([int]2835); $bw.Write([int]2835); $bw.Write([int]0); $bw.Write([int]0)
        $bw.Write($px, 0, $img); $bw.Dispose()
        W ("saved {0} ({1} x {2}, frame {3})" -f $Bmp, $w, $h, $n); $saved = $true
        $sb.Dispose()
      }
    }
    $fr.Dispose()
  }
  $fps = $n / $Seconds
  W ("frames: {0} in {1} s = {2:N1} fps; first after {3:N0} ms; SoftwareBitmap {4}, Direct3DSurface {5}" -f $n, $Seconds, $fps, $first, $sw, $d3d)
} catch {
  W ('FAILED: ' + (Err $_))
} finally {
  if ($reader) { try { AwaitAction ($reader.StopAsync()) } catch { W ('StopAsync: ' + (Err $_)) }; $reader.Dispose() }
  if ($mc) { $mc.Dispose() }
}
W "log: $Out"
