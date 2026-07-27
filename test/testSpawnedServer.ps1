# qiv-remote.ps1 — interactive test client for the qIV TCP remote control.
#
# Pure .NET sockets, nothing to install, nothing Defender objects to.
#
#   .\qiv-remote.ps1                          connect to 127.0.0.1:8770
#   .\qiv-remote.ps1 -Port 9000               different port
#   .\qiv-remote.ps1 -Host 192.168.1.50       another machine
#   .\qiv-remote.ps1 -Command next            send one command and exit
#
# Type commands at the prompt; blank line or 'exit' quits.

[CmdletBinding()]
param(
    [string]$HostName = '127.0.0.1',
    [int]$Port = 8770,
    [string]$Command = ''
)

function Read-Available {
    param($Reader, [int]$FirstTimeoutMs = 2000)

    # qIV answers most commands with a single OK/ERR line, but 'help' emits a
    # listing first. So keep reading until a line starts with OK or ERR, and let
    # a timeout end it if the server sends nothing at all (which is what a
    # passwordless server does after its banner).
    $lines = @()
    try {
        $Reader.BaseStream.ReadTimeout = $FirstTimeoutMs
        while ($true) {
            $line = $Reader.ReadLine()
            if ($null -eq $line) { break }
            $lines += $line
            if ($line -match '^(OK|ERR)\b') { break }
        }
    } catch [System.IO.IOException] {
        # read timeout — normal when the server has nothing more to say
    }
    return $lines
}

try {
    $client = [System.Net.Sockets.TcpClient]::new()
    $connect = $client.ConnectAsync($HostName, $Port)
    if (-not $connect.Wait(4000)) { throw "connect timed out" }
} catch {
    Write-Host "Could not connect to ${HostName}:${Port} - $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$stream = $client.GetStream()
$reader = [System.IO.StreamReader]::new($stream)
$writer = [System.IO.StreamWriter]::new($stream)
$writer.AutoFlush = $true

# Banner, then possibly an AUTH challenge.
foreach ($l in (Read-Available $reader)) {
    if ($l -match '^AUTH ') {
        Write-Host "Server requires a password. This script does not implement" -ForegroundColor Yellow
        Write-Host "challenge-response - use the qIV remote panel to connect." -ForegroundColor Yellow
        $client.Close()
        exit 1
    }
    Write-Host $l -ForegroundColor DarkGray
}

if ($Command) {
    $writer.WriteLine($Command)
    Read-Available $reader | ForEach-Object { Write-Host $_ }
    $client.Close()
    exit 0
}

Write-Host "Connected to ${HostName}:${Port}. Blank line or 'exit' to quit." -ForegroundColor Green

while ($client.Connected) {
    $cmd = Read-Host 'qiv'
    if ([string]::IsNullOrWhiteSpace($cmd) -or $cmd -eq 'exit') { break }

    try {
        $writer.WriteLine($cmd)
    } catch {
        Write-Host 'Connection lost.' -ForegroundColor Red
        break
    }

    foreach ($l in (Read-Available $reader)) {
        $colour = if ($l -match '^ERR') { 'Red' } elseif ($l -match '^OK') { 'Green' } else { 'Gray' }
        Write-Host $l -ForegroundColor $colour
    }
}

$client.Close()
Write-Host 'Disconnected.' -ForegroundColor DarkGray
