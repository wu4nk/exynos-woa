<#
  kit-watch.ps1 (v1.0) - runs at every logon (scheduled task "S10 kit watcher", elevated, in the user's session).
  If push-to-windows.cmd left a NEW kit on the EFI partition (ESP:\S10KIT, written from TWRP into /cache), it asks
  "install it?"; Yes = copy to C:\S10Prov\kit, run install-in-windows.ps1 in a window you can read, remove the kit
  from the ESP. Drivers are replaced live (pnputil /install): no restart needed unless Windows asks for one.
  Nothing on the ESP, or the same kit already installed: it exits silently. Log: C:\S10Prov\kit-watch.log
#>
$ErrorActionPreference = 'Continue'
$log = 'C:\S10Prov\kit-watch.log'
New-Item -ItemType Directory -Force C:\S10Prov | Out-Null
function L($m) { "$(Get-Date -Format s)  $m" | Add-Content -Path $log -Encoding utf8 }
Add-Type -AssemblyName PresentationFramework

Start-Sleep -Seconds 8                                   # let the desktop come up first
$letter = @('S', 'R', 'Q', 'P', 'O', 'N') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
if (-not $letter) { L 'no free drive letter for the ESP'; exit 0 }
mountvol "${letter}:" /s 2>$null | Out-Null
if (-not (Test-Path "${letter}:\EFI")) { L "mountvol ${letter}: /s failed"; mountvol "${letter}:" /d 2>$null | Out-Null; exit 0 }

try {
  $k = "${letter}:\S10KIT"
  if (-not (Test-Path "$k\out")) { L 'no S10KIT on the ESP'; return }

  $stamp = ((Get-Content "$k\kit.stamp" -ErrorAction SilentlyContinue) -join '').Trim()
  if (-not $stamp) { $stamp = 'size' + (Get-ChildItem "$k\out" -Recurse -File | Measure-Object Length -Sum).Sum }
  $done = ((Get-Content 'C:\S10Prov\kit\kit.stamp' -ErrorAction SilentlyContinue) -join '').Trim()
  if ($stamp -eq $done) { L "kit $stamp already installed - removing it from the ESP"; Remove-Item $k -Recurse -Force; return }

  $vers = foreach ($i in Get-ChildItem "$k\out" -Recurse -Filter *.inf) {
    $v = (Select-String -Path $i.FullName -Pattern '^\s*DriverVer\s*=\s*(.+)$' | Select-Object -First 1).Matches.Groups[1].Value
    "  {0}  {1}" -f $i.BaseName, (("$v" -split ',')[-1]).Trim()
  }
  $msg = "Co bo driver moi tren phan vung EFI (S10KIT, $stamp):`n`n$($vers -join "`n")`n`nCai ngay bay gio? (khong can khoi dong lai)"
  $r = [System.Windows.MessageBox]::Show($msg, 'Galaxy S10 - driver moi', 'YesNo', 'Question')
  if ("$r" -ne 'Yes') { L "user declined kit $stamp (asked again next logon)"; return }

  L "installing kit $stamp"
  robocopy $k C:\S10Prov\kit /E /R:1 /W:1 /XF kit.stamp INSTALL.CMD /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { L "robocopy failed ($LASTEXITCODE)"; [System.Windows.MessageBox]::Show("Chep kit sang C:\S10Prov\kit loi (robocopy $LASTEXITCODE).", 'Galaxy S10', 'OK', 'Error') | Out-Null; return }

  $p = Start-Process powershell.exe -PassThru -Wait -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
        "& 'C:\S10Prov\kit\install-in-windows.ps1'; `$c = `$LASTEXITCODE; Write-Host ''; Read-Host 'Xong - Enter de dong cua so'; exit `$c")
  L "install-in-windows.ps1 exit $($p.ExitCode)"
  if ($p.ExitCode -eq 0) {
    Set-Content -Path 'C:\S10Prov\kit\kit.stamp' -Value $stamp -Encoding ascii
    Remove-Item $k -Recurse -Force -ErrorAction SilentlyContinue
    L "kit $stamp installed, removed from the ESP"
  } else {
    [System.Windows.MessageBox]::Show("Cai driver loi (exit $($p.ExitCode)). Bo kit van con tren ESP, lan dang nhap sau se hoi lai.`nLog: C:\S10Prov\install.log", 'Galaxy S10', 'OK', 'Warning') | Out-Null
  }
} finally {
  mountvol "${letter}:" /d 2>$null | Out-Null
}
