<#
  stay-s10.ps1 (v1.0) - Galaxy S10: stop the "one extra trip to TWRP" after a Windows run that did not end with a
  Windows restart (notes/beyond1-port.md "boot to recovery once").
  Run ON THE PHONE, PowerShell as Administrator:
      .\stay-s10.ps1            = .\stay-s10.ps1 on
      .\stay-s10.ps1 on         put \S10EDIAG\stay.flg on the ESP the firmware reads (the FAT volume with
                                \EFI\Microsoft\Boot\BCD): the next boot after a shutdown / crash / forced reset goes
                                straight back into Windows (its log is still saved to \WINPRAM.BIN)
      .\stay-s10.ps1 off        remove it again (the developer loop: every such boot ends in TWRP)
      .\stay-s10.ps1 status     show whether it is there + the firmware's boot-pair lines from \UEFILOG.TXT
  Why: the UEFI (Beyond0LteBootLogDxe "W23 boot pair") leaves \WINRUN.FLG on the ESP before starting Windows. If the
  next boot finds it and Windows did not ask for that reset itself (shutdown + power on, a blue screen, a hang, holding
  Vol Down + Power), it saves the evidence and reboots into recovery - unless \S10EDIAG\stay.flg is there. The S10e's
  interactive setup always had stay.flg (HANDOFF §29); the S10's kit never created it.
  Safety kept: with stay.flg, the 2nd Windows death in a row (no cold boot / clean restart between) still goes to TWRP.
  Writes C:\S10Prov\stay-s10.txt.
#>
param([Parameter(Position = 0)][ValidateSet('on', 'off', 'status')][string]$Action = 'on')
$ErrorActionPreference = 'Continue'
$Out = 'C:\S10Prov\stay-s10.txt'
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
"stay-s10.ps1 v1.0  $Action  $(Get-Date -Format s)" | Set-Content $Out -Encoding ascii
function W($m) { Write-Host $m; "$m" | Add-Content $Out -Encoding ascii }

# The firmware's ESP = the FAT volume that carries \EFI\Microsoft\Boot\BCD (OpenEspRoot). Windows does not letter
# it: mountvol /s gives it a letter for the run. A dual-boot phone has more than one FAT partition, so every FAT
# volume is checked (gpu-s10.ps1 v1.21's lesson) and only the one(s) with the BCD are used.
$roots = @()
foreach ($d in Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue) { $roots += $d.Root }
foreach ($v in @(Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemType -like 'FAT*' -and $_.Path })) { $roots += $v.Path }
$letter = $null
if (-not ($roots | Where-Object { [IO.File]::Exists($_ + 'EFI\Microsoft\Boot\BCD') })) {
  $letter = @('S', 'R', 'Q', 'P', 'O') | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
  if ($letter) { mountvol "${letter}:" /s 2>$null; if (Test-Path "${letter}:\") { $roots += "${letter}:\" } else { $letter = $null } }
}
$esps = @($roots | Select-Object -Unique | Where-Object { [IO.File]::Exists($_ + 'EFI\Microsoft\Boot\BCD') })
if (-not $esps) {
  W '[FAIL] no FAT volume with \EFI\Microsoft\Boot\BCD found (run as Administrator?) - nothing changed'
  if ($letter) { mountvol "${letter}:" /d 2>$null }
  exit 1
}

foreach ($r in $esps) {
  $flag = $r + 'S10EDIAG\stay.flg'
  switch ($Action) {
    'on' {
      try {
        [IO.Directory]::CreateDirectory($r + 'S10EDIAG') | Out-Null
        if (-not [IO.File]::Exists($flag)) { [IO.File]::WriteAllText($flag, "stay-s10.ps1 $(Get-Date -Format s)`r`n") }
        W "[ OK ] $flag present: after a shutdown / crash the phone boots straight back into Windows"
      } catch { W "[FAIL] ${flag}: $($_.Exception.Message)" }
    }
    'off' {
      try {
        if ([IO.File]::Exists($flag)) { [IO.File]::Delete($flag); W "[ OK ] $flag removed (developer loop: such boots end in TWRP)" }
        else { W "[ OK ] no $flag (already the developer loop)" }
      } catch { W "[FAIL] ${flag}: $($_.Exception.Message)" }
    }
    'status' {
      W ("ESP {0}: stay.flg {1}, WINRUN.FLG {2}" -f $r, $(if ([IO.File]::Exists($flag)) { 'PRESENT' } else { 'absent' }),
        $(if ([IO.File]::Exists($r + 'WINRUN.FLG')) { 'present (Windows is running = normal)' } else { 'absent' }))
      foreach ($f in 'WINPRAM.BIN', 'WINPRAM-RESTART.BIN', 'UEFILOG.TXT') {
        $p = $r + $f
        if ([IO.File]::Exists($p)) { W ("  {0,-20} {1:yyyy-MM-dd HH:mm:ss}" -f $f, (Get-Item $p).LastWriteTime) }
      }
      $log = $r + 'UEFILOG.TXT'
      if ([IO.File]::Exists($log)) {
        W '  boot-pair lines in UEFILOG.TXT:'
        Get-Content $log -ErrorAction SilentlyContinue | Select-String -Pattern 'BootLog: (previous boot|Windows asked|clean Windows|Windows died)|stay\.flg' |
          Select-Object -Last 10 | ForEach-Object { W ("    " + $_.Line.Trim()) }
      }
    }
  }
}
if ($letter) { mountvol "${letter}:" /d 2>$null }
W "log: $Out"
