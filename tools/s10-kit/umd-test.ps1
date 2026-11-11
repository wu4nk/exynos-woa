<#
  umd-test.ps1 (v1.0) - does maliumd.dll LOAD on this phone? Safe: runs in the normal (display-only) mode, no
  driver or registry change, no reboot. Run ON THE PHONE, PowerShell (Admin not needed):
    .\umd-test.ps1                      tests C:\Windows\System32\maliumd.dll
    .\umd-test.ps1 C:\path\maliumd.dll  tests another copy
  Why: in the G2a full-mode boots DWM crash-looped (dwmcore 0x8898008D, Dwminit on "Arm Mali-G76") and NONE of the
  47 WER reports lists maliumd.dll among DWM's modules - the D3D runtime never got our UMD loaded. This tells
  apart "the DLL does not load" (missing import / DLL init failed / wrong machine) from "it loads, OpenAdapter fails".
  Result -> C:\S10Prov\umd-test.txt (and on screen).
#>
param([string]$Dll = "$env:SystemRoot\System32\maliumd.dll")
$ErrorActionPreference = 'Continue'
$Out = 'C:\S10Prov\umd-test.txt'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
"umd-test v1.0  $(Get-Date -Format s)" | Set-Content $Out -Encoding ascii
function Log($m) { Write-Host $m; $m | Add-Content $Out }

Log "process: arch $env:PROCESSOR_ARCHITECTURE  64-bit $([Environment]::Is64BitProcess)  (must be ARM64 / True)"
if (-not (Test-Path $Dll)) { Log "FAIL: $Dll does not exist"; exit 1 }
$fi = Get-Item $Dll
Log ("dll: {0}  {1:N0} bytes  {2}  sha256 {3}" -f $Dll, $fi.Length, $fi.LastWriteTime.ToString('s'), (Get-FileHash $Dll).Hash.ToLower())

# a second run in the same PowerShell window must not add the type again (TYPE_ALREADY_EXISTS stops the script)
if (-not ('S10.K' -as [type])) {
Add-Type -Namespace S10 -Name K -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern System.IntPtr LoadLibraryExW(string name, System.IntPtr file, uint flags);
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
public static extern System.IntPtr GetProcAddress(System.IntPtr mod, string name);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool FreeLibrary(System.IntPtr mod);
'@
}

# 1. map only (no DllMain, no imports): is it a valid image for this machine?
$h = [S10.K]::LoadLibraryExW($Dll, [IntPtr]::Zero, 0x2)   # LOAD_LIBRARY_AS_DATAFILE
$e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
if ($h -eq [IntPtr]::Zero) { Log "map as data: FAIL error $e" } else { Log 'map as data: OK'; [S10.K]::FreeLibrary($h) | Out-Null }

# 2. real load: imports resolved + mingw CRT DllMain + TLS
$h = [S10.K]::LoadLibraryExW($Dll, [IntPtr]::Zero, 0x0)
$e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
if ($h -eq [IntPtr]::Zero) {
  $msg = (New-Object ComponentModel.Win32Exception $e).Message
  Log "LOAD: FAIL error $e ($msg)"
  Log '  126 = a DLL it imports is missing, 127 = an imported function is missing, 193 = wrong machine type,'
  Log '  1114 = DllMain/TLS init failed, 577/1455/... = code integrity / resources'
  exit 2
}
Log ('LOAD: OK at 0x{0:X}' -f $h.ToInt64())
foreach ($f in 'OpenAdapter10', 'OpenAdapter10_2') {
  $p = [S10.K]::GetProcAddress($h, $f)
  if ($p -eq [IntPtr]::Zero) { Log "  export $f : MISSING" } else { Log ('  export {0} : OK 0x{1:X}' -f $f, $p.ToInt64()) }
}
Log 'modules loaded into this process from System32 because of it:'
(Get-Process -Id $PID).Modules | Where-Object { $_.ModuleName -match 'maliumd|ucrt|api-ms-win-crt|vcruntime|msvcp|libc\+\+|gdi32' } |
  ForEach-Object { Log "  $($_.ModuleName)  $($_.FileName)" }
Log "result -> $Out"
