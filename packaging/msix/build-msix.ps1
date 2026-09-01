# Build the MSIX package for the Microsoft Store.
#
# WHY MSIX AND NOT AN EXE SUBMISSION. The Store's EXE/MSI route requires a code
# signing certificate from a CA in the Microsoft Trusted Root Program - a
# recurring purchase - and a SILENT INSTALLER. qIV has no installer at all, and
# "one portable EXE, no installer" is the thing it is known for. MSIX is signed
# and hosted by Microsoft for free, so the portable EXE on GitHub keeps shipping
# exactly as it does and this is an additional wrapper, never a replacement.
#
#   .\build-msix.ps1                       package only, unsigned
#   .\build-msix.ps1 -SelfSign             package and sign for LOCAL testing
#
# The unsigned package is what goes to Partner Center: the Store signs it. Self
# signing is only for installing it on this machine to check it actually runs.
#
# PREREQUISITE: Release_Static must be built first. The Store copy has to be the
# same statically linked binary that ships on GitHub - a package carrying a
# dynamic-CRT build would need the redistributable that this project's whole
# pitch says you do not have to install.

[CmdletBinding()]
param(
    [string] $ExePath = 'I:\30_CppSources\qiv-relstatic\QuickImageViewer.exe',
    [string] $OutFile = 'I:\30_CppSources\qiv-relstatic\QuickImageViewer.msix',
    [switch] $SelfSign
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# The newest SDK on the machine, rather than a version pinned in this file that
# goes stale the first time the SDK updates.
$kit = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory |
       Where-Object { Test-Path (Join-Path $_.FullName 'x64\makeappx.exe') } |
       Sort-Object Name -Descending | Select-Object -First 1
if (-not $kit) { throw 'makeappx.exe not found - install the Windows SDK.' }
$makeappx = Join-Path $kit.FullName 'x64\makeappx.exe'
$signtool = Join-Path $kit.FullName 'x64\signtool.exe'
Write-Host "  SDK      $($kit.Name)"

if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath - build Release_Static first." }

# ⚠ STAGED INTO A CLEAN FOLDER, NEVER PACKED FROM $here.
#
# makeappx /d sweeps EVERYTHING under the directory it is given, and the first
# build of this package shipped build-msix.ps1 to the Microsoft Store inside the
# submission. The exe is also 10 MB of build output that has no business sitting
# beside the manifest in git. Staging fixes both: the package contains exactly
# the manifest, the artwork and the binary, and nothing that happens to share a
# folder with them.
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('qiv-msix-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    Copy-Item (Join-Path $here 'AppxManifest.xml') $stage
    Copy-Item (Join-Path $here 'Assets') $stage -Recurse
    Copy-Item $ExePath (Join-Path $stage 'QuickImageViewer.exe')

    & $makeappx pack /d $stage /p $OutFile /o
    if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}

if ($SelfSign) {
    Write-Host '  signing with a throwaway certificate - LOCAL TESTING ONLY'
    $subject = 'CN=PLACEHOLDER'
    $cert = New-SelfSignedCertificate -Type Custom -Subject $subject `
        -KeyUsage DigitalSignature -FriendlyName 'qIV MSIX local test' `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')

    # ⚠ The certificate SUBJECT must equal the manifest's Publisher exactly, or
    # signing succeeds and installation fails with an error that blames the
    # package rather than the mismatch.
    $manifestPublisher = ([xml](Get-Content (Join-Path $here 'AppxManifest.xml'))).Package.Identity.Publisher
    if ($manifestPublisher -ne $subject) {
        throw "Manifest Publisher is '$manifestPublisher' but the test certificate is '$subject'."
    }

    $pfx = Join-Path $env:TEMP 'qiv-msix-test.pfx'
    $pw  = ConvertTo-SecureString -String 'qivtest' -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null
    & $signtool sign /fd SHA256 /f $pfx /p qivtest $OutFile
    Remove-Item $pfx -Force -ErrorAction SilentlyContinue

    Write-Host ''
    Write-Host '  To install for testing, trust the certificate first:'
    Write-Host '    Export-Certificate then Import-Certificate into Cert:\LocalMachine\TrustedPeople'
    Write-Host '    Add-AppxPackage -Path ' + $OutFile
    Write-Host ''
    Write-Host '  ⚠ AFTERWARDS, remove BOTH: Remove-AppxPackage, and delete the'
    Write-Host '    CN=PLACEHOLDER certificate from TrustedPeople and CurrentUser\My.'
    Write-Host '    A trusted self-signed certificate left on the machine is a real'
    Write-Host '    hole, not untidiness.'
}

Write-Host ''
Write-Host "  wrote $OutFile"
Write-Host ''
Write-Host '  Identity is set from Partner Center and must not be edited:'
Write-Host '    IVANHRISTOVYANEV.QuickImageViewer / CN=6B94F428-...'
Write-Host ''
Write-Host '  Upload this file UNSIGNED. The Store signs it.'
