<#
.SYNOPSIS
  Creates the self-signed DEVELOPMENT signing certificate for the host package.

.DESCRIPTION
  Output (git-ignored):  host/signing/xbox-rl-dev.pfx  and  host/signing/xbox-rl-dev.cer
  Subject must equal the manifest Publisher ("CN=xbox-rl").
  The PFX password is the fixed, NON-secret string 'xrl-dev' (a throwaway dev cert whose
  only job is to make Device Portal accept the package; it protects nothing).
  The certificate is generated in memory, exported, and removed from the user's cert
  store again - nothing is added to any trust store by this script. deploy.ps1 uploads
  the .cer to the console, which is what makes the console trust the package.
#>
[CmdletBinding()]
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $PSScriptRoot '..\host\signing'
New-Item -ItemType Directory -Force $dir | Out-Null
$pfx = Join-Path $dir 'xbox-rl-dev.pfx'
$cer = Join-Path $dir 'xbox-rl-dev.cer'
if ((Test-Path $pfx) -and -not $Force) { Write-Host "[cert] exists: $pfx (use -Force to recreate)"; return }

$cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=xbox-rl' -KeyUsage DigitalSignature `
    -FriendlyName 'xbox-rl dev signing' -CertStoreLocation 'Cert:\CurrentUser\My' `
    -NotAfter (Get-Date).AddYears(5) `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
try {
    $pw = ConvertTo-SecureString 'xrl-dev' -AsPlainText -Force
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null
    Export-Certificate    -Cert $cert -FilePath $cer | Out-Null
} finally {
    Remove-Item -Path "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force
}
Write-Host "[cert] created $pfx and $cer (thumbprint $($cert.Thumbprint)); removed from the user cert store."
