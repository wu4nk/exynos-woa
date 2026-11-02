<#
  get-ddi-headers.ps1 - gom cac header D3D DDI (chi co trong WDK/EWDK) thanh 1 zip nho de gui cho Claude,
  de Claude build Mesa (panfrost + d3d10umd, ARM64) tren may Linux (G2, phuong an 2).

  Chay tren PC (khong phai tren dien thoai), sau khi da mount ISO EWDK:
    powershell -ExecutionPolicy Bypass -File get-ddi-headers.ps1
  hoac chi ro thu muc Include:
    powershell -ExecutionPolicy Bypass -File get-ddi-headers.ps1 -Include "E:\Program Files\Windows Kits\10\Include\10.0.26100.0"

  Ket qua: Desktop\ddi-headers.zip (~1 MB) - keo tha zip do vao chat.
  Chi lay header (khai bao kieu/struct), khong co .lib, khong co binary.
#>
param([string]$Include)

$ErrorActionPreference = 'Stop'

if (-not $Include) {
  $roots = @()
  foreach ($d in Get-PSDrive -PSProvider FileSystem) {
    $roots += Join-Path $d.Root 'Program Files\Windows Kits\10\Include'
    $roots += Join-Path $d.Root 'Program Files (x86)\Windows Kits\10\Include'
    $roots += Join-Path $d.Root 'Windows Kits\10\Include'
  }
  $cands = foreach ($r in $roots) {
    if (Test-Path $r) {
      Get-ChildItem $r -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\d3d10umddi.h') }
    }
  }
  if (-not $cands) { throw 'Khong tim thay d3d10umddi.h. Mount ISO EWDK roi chay lai, hoac dung -Include "<...>\Include\<ver>".' }
  $Include = ($cands | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).FullName
}
Write-Host "Include: $Include"

# Mesa include/winddk/README.txt list first, then neighbours they may include.
$want = @('d3d10tokenizedprogramformat.hpp', 'd3d11tokenizedprogramformat.hpp',
          'd3d10umddi.h', 'd3d10_1umddi.h', 'd3d11umddi.h', 'd3dumddi.h', 'dxgiddi.h', 'dxmini.h', 'wmidata.h',
          'd3dkmthk.h', 'd3dukmdt.h', 'd3dkmdt.h', 'd3dkmddi.h', 'dxgitype.h', 'dxgiformat.h',
          'd3d12umddi.h', 'dispmprt.h', 'video.h', 'videoagp.h', 'ntddvdeo.h', 'iddcx.h',
          'd3d10.h', 'd3d10_1.h', 'd3d11.h', 'd3d10misc.h', 'd3d10shader.h', 'd3d10sdklayers.h',
          'd3dcommon.h', 'd3d9types.h', 'd3d9caps.h', 'dxgi.h', 'dxgicommon.h', 'sal.h',
          'specstrings.h', 'specstrings_strict.h', 'driverspecs.h', 'concurrencysal.h')

$out = Join-Path $env:TEMP 'ddi-headers'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
$n = 0
foreach ($sub in 'um', 'shared', 'km') {
  $src = Join-Path $Include $sub
  if (-not (Test-Path $src)) { continue }
  New-Item -ItemType Directory -Force (Join-Path $out $sub) | Out-Null
  foreach ($h in $want) {
    $f = Join-Path $src $h
    if (Test-Path $f) { Copy-Item $f (Join-Path $out $sub); $n++; Write-Host "  $sub\$h" }
  }
}
(Split-Path $Include -Leaf) | Set-Content (Join-Path $out 'SDK-VERSION.txt')
$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) 'ddi-headers.zip'
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$out\*" -DestinationPath $zip
Write-Host ("OK: {0} file -> {1} ({2:N0} KB)" -f $n, $zip, ((Get-Item $zip).Length / 1KB))
Write-Host 'Keo tha ddi-headers.zip vao chat la xong.'
