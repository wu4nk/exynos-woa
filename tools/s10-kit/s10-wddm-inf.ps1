<#
s10-wddm-inf.ps1 (v1.0) - exynos-woa S10 kit: fit upstream's MaliG76Wddm.inf to the user-mode driver files the S10 kit
actually ships.

    s10-wddm-inf.ps1 <package-dir>\MaliG76Wddm.inf

Upstream's INF (1.0.72.1) carries seven user-mode files: maliumd.dll (ARM64X forwarder), maliumd_arm64.dll,
maliumd_ec.dll (ARM64EC), maliumd32.dll (x86, WOW64) and the OpenGL ICD maligl.dll / maligl_arm64.dll / maligl_ec.dll.
They are built on upstream's Windows PC (clang-cl + MSVC). The S10 builds its UMD on Linux (zig / mingw): so far only
the ARM64 one. inf2cat and pnputil refuse a package that lists a file it does not contain, so every file that is NOT
in the package folder is taken out of [SourceDisksFiles] and the copy sections; the registry values that name it go
too (UserModeDriverNameWow without maliumd32.dll, OpenGLDriverName/Version/Flags without maligl.dll). An emptied copy
section is dropped from CopyFiles and DestinationDirs. With every file present the INF is left as it is.
#>
param([Parameter(Mandatory = $true)][string]$Inf)
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $Inf
$files = 'maliumd.dll', 'maliumd32.dll', 'maliumd_arm64.dll', 'maliumd_ec.dll', 'maligl.dll', 'maligl_arm64.dll', 'maligl_ec.dll'
$missing = @($files | Where-Object { -not (Test-Path (Join-Path $dir $_)) })
if ($missing.Count -eq 0) { Write-Host '[ OK ] MaliG76Wddm.inf: every user-mode file present, INF unchanged'; exit 0 }

$lines = Get-Content -LiteralPath $Inf
$out = New-Object System.Collections.Generic.List[string]
$section = ''
foreach ($l in $lines) {
    $t = $l.Trim()
    if ($t -match '^\[(.+)\]') { $section = $Matches[1] }
    $name = ($t -split '[,=;\s]', 2)[0]
    if ($missing -contains $name -and ($section -eq 'SourceDisksFiles' -or $section -like 'MW_Umd*')) { continue }
    if ($missing -contains 'maliumd32.dll' -and $t -like 'HKR,,UserModeDriverNameWow,*') { continue }
    if ($missing -contains 'maligl.dll' -and $t -match '^HKR,,OpenGL(DriverName|Version|Flags),') { continue }
    $out.Add($l)
}
# copy sections left without a file
$empty = @()
foreach ($s in 'MW_UmdFiles', 'MW_UmdWow') {
    $i = $out.IndexOf(($out | Where-Object { $_.Trim() -eq "[$s]" } | Select-Object -First 1))
    if ($i -lt 0) { continue }
    $has = $false
    for ($j = $i + 1; $j -lt $out.Count -and -not $out[$j].Trim().StartsWith('['); $j++) {
        $x = $out[$j].Trim(); if ($x -and -not $x.StartsWith(';')) { $has = $true }
    }
    if (-not $has) { $empty += $s }
}
for ($k = 0; $k -lt $out.Count; $k++) {
    foreach ($s in $empty) {
        if ($out[$k].Trim() -match "^$s\s*=") { $out[$k] = "; $s dropped by s10-wddm-inf.ps1 (no file)" }
        if ($out[$k] -match '^\s*CopyFiles\s*=') { $out[$k] = ($out[$k] -replace ",\s*$s\b", '') }
    }
}
Set-Content -LiteralPath $Inf -Value $out -Encoding ASCII
Write-Host ("[ OK ] MaliG76Wddm.inf fitted to the S10 kit: without {0}" -f ($missing -join ', '))
