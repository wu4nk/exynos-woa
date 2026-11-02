<#
  auto-install.ps1 (v1.0) - started elevated by S10-install-once.cmd (StartUp folder) after
  push-to-windows put the kit in C:\S10Prov\kit from TWRP. Runs install-in-windows.ps1 with a
  transcript, marks success, reboots once so the new drivers bind.
#>
$ErrorActionPreference = 'Continue'
$kit = $PSScriptRoot
Start-Transcript -Path 'C:\S10Prov\install.log' -Append | Out-Null
Write-Host "S10 kit auto-install $(Get-Date -Format s) from $kit"
& (Join-Path $kit 'install-in-windows.ps1')
$rc = $LASTEXITCODE
if ($rc -eq 0 -or $rc -eq $null) {
  Set-Content (Join-Path $kit 'install.done') (Get-Date -Format s)
  Write-Host '[ OK ] install done - rebooting in 20 s (shutdown /a cancels)' -ForegroundColor Green
  Stop-Transcript | Out-Null
  shutdown /r /t 20 /c "S10 kit: drivers installed, restarting"
} else {
  Write-Host "[FAIL] install-in-windows.ps1 exit $rc - log: C:\S10Prov\install.log" -ForegroundColor Red
  Stop-Transcript | Out-Null
  Start-Sleep 30
}
