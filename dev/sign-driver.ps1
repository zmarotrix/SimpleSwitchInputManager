# Signs the WinUHid driver package with a certificate made on this PC.
# No admin rights needed. The private key is deleted afterwards, so nothing can
# ever be signed with this certificate again (re-run this script to make a new one).

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src = Join-Path $root '..\third_party\WinUHid\WinUHid Driver\build\Release\x64\WinUHid Driver'
$out = Join-Path $root 'driver'
$signtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'

New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $src 'WinUHidDriver.inf'), (Join-Path $src 'WinUHidDriver.dll'), (Join-Path $src 'winuhiddriver.cat') $out -Force

$cert = New-SelfSignedCertificate -Type CodeSigningCert `
    -Subject 'CN=Switch2Pro Local Driver Signing' `
    -CertStoreLocation Cert:\CurrentUser\My `
    -KeyExportPolicy NonExportable `
    -NotAfter (Get-Date).AddYears(20)

try {
    & $signtool sign /q /fd sha256 /sha1 $cert.Thumbprint (Join-Path $out 'WinUHidDriver.dll')
    if ($LASTEXITCODE) { throw 'signing the driver DLL failed' }
    & $signtool sign /q /fd sha256 /sha1 $cert.Thumbprint (Join-Path $out 'winuhiddriver.cat')
    if ($LASTEXITCODE) { throw 'signing the catalog failed' }
    Export-Certificate -Cert $cert -FilePath (Join-Path $out 'Switch2Pro-signing.cer') | Out-Null
}
finally {
    # Delete the certificate and its private key.
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
}

Write-Host "Driver package signed in $out (certificate thumbprint $($cert.Thumbprint))"
