<#
  new-cert.ps1 - make the z3s test-signing certificate (run once, normal
  PowerShell, no admin needed):
    powershell -ExecutionPolicy Bypass -File new-cert.ps1

  SHA-256 / RSA 2048 code-signing certificate in CurrentUser\My. The old
  makecert certificate (SHA-1, PrivateCertStore) is rejected by the EWDK
  10.0.28000 signtool: "The signer's certificate is not valid for signing".

  It is also added to CurrentUser Root + TrustedPublisher so signtool can
  build the chain (Windows asks once to confirm the Root add: answer Yes).
  It is only trusted on this PC, for this user.
#>
$ErrorActionPreference = 'Stop'
$name = 'CN=z3s WoA Test'
$dir  = 'C:\z3s-cert'

# remove old copies so "signtool /n" finds exactly one
foreach ($store in 'My', 'PrivateCertStore') {
  Get-ChildItem "Cert:\CurrentUser\$store" -ErrorAction SilentlyContinue |
    Where-Object Subject -eq $name | Remove-Item -ErrorAction SilentlyContinue
}

$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $name `
  -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
  -KeyUsage DigitalSignature -KeySpec Signature `
  -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(10)

New-Item -ItemType Directory -Force $dir | Out-Null
$cer = Join-Path $dir 'z3s-test.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\CurrentUser\TrustedPublisher | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\CurrentUser\Root | Out-Null

Write-Host "[ OK ] $name  thumbprint $($cert.Thumbprint)" -ForegroundColor Green
Write-Host "       exported to $cer; now run build-all.cmd"
