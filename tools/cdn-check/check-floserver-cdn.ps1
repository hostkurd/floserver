# check-floserver-cdn.ps1
# Checks every runtime file on cdn.floserver.org:
#   1. the CDN serves it (HTTP 200, through CloudFront)
#   2. its SHA-256 matches the checksum the official project publishes
#      (nginx and Apache publish none, so their originals are downloaded and compared)
# Then checks that the S3 bucket can't be read directly (pass -Bucket).
#
# Run in PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\check-floserver-cdn.ps1 -Bucket <bucket-name> -Region <region>
#
# It downloads about 800 MB into a temporary folder and deletes it at the end.
# Two reports are saved next to this script:
#   floserver-cdn-report.txt   to read
#   floserver-cdn-report.json  for: floserver-manifest build floserver-cdn-report.json --sequence N
#
# To publish a new runtime version: upload it to the CDN, add a line to $files
# below with its official checksum list, run this script, then build and sign.

param(
    [string]$Cdn = "https://cdn.floserver.org",
    [string]$Bucket = "",
    [string]$Region = "",
    [string]$Only = ""   # optional: check only paths containing this text, e.g. "php/"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is very slow with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Each file: path on the CDN, and how to find its official SHA-256.
#   Sums  = URL of the official checksum list (the line naming the file is used)
#   Orig  = URL of the original file, downloaded and hashed when there is no list
$files = @(
    @{ Path = "php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip"; Sums = "https://downloads.php.net/~windows/releases/sha256sum.txt" },
    @{ Path = "php/8.4.27/php-8.4.27-1-nts-Win32-vs17-x64.zip"; Sums = "https://downloads.php.net/~windows/releases/sha256sum.txt" },
    @{ Path = "php/8.3.36/php-8.3.36-1-nts-Win32-vs16-x64.zip"; Sums = "https://downloads.php.net/~windows/releases/sha256sum.txt" },
    @{ Path = "php/8.2.35/php-8.2.35-1-nts-Win32-vs16-x64.zip"; Sums = "https://downloads.php.net/~windows/releases/sha256sum.txt" },
    @{ Path = "mariadb/11.4.13/mariadb-11.4.13-winx64.zip";     Sums = "https://archive.mariadb.org/mariadb-11.4.13/winx64-packages/sha256sums.txt" },
    @{ Path = "mariadb/11.4.13/mariadb-11.4.13.tar.gz";         Sums = "https://archive.mariadb.org/mariadb-11.4.13/source/sha256sums.txt" },
    @{ Path = "mariadb/11.8.9/mariadb-11.8.9-winx64.zip";       Sums = "https://archive.mariadb.org/mariadb-11.8.9/winx64-packages/sha256sums.txt" },
    @{ Path = "mariadb/11.8.9/mariadb-11.8.9.tar.gz";           Sums = "https://archive.mariadb.org/mariadb-11.8.9/source/sha256sums.txt" },
    @{ Path = "node/24.22.1/node-v24.22.1-win-x64.zip";         Sums = "https://nodejs.org/dist/v24.22.1/SHASUMS256.txt" },
    @{ Path = "node/22.23.4/node-v22.23.4-win-x64.zip";         Sums = "https://nodejs.org/dist/v22.23.4/SHASUMS256.txt" },
    @{ Path = "composer/2.11.0/composer.phar";                  Sums = "https://getcomposer.org/download/2.11.0/composer.phar.sha256sum" },
    @{ Path = "composer/2.10.3/composer.phar";                  Sums = "https://getcomposer.org/download/2.10.3/composer.phar.sha256sum" },
    @{ Path = "phpmyadmin/5.2.3/phpMyAdmin-5.2.3-all-languages.zip"; Sums = "https://files.phpmyadmin.net/phpMyAdmin/5.2.3/phpMyAdmin-5.2.3-all-languages.zip.sha256" },
    @{ Path = "nginx/1.30.6/nginx-1.30.6.zip";                  Orig = "https://nginx.org/download/nginx-1.30.6.zip" },
    @{ Path = "wordpress/7.1.4/wordpress-7.1.4.zip";            Orig = "https://wordpress.org/wordpress-7.1.4.zip" },
    @{ Path = "wordpress/7.0.8/wordpress-7.0.8.zip";            Orig = "https://wordpress.org/wordpress-7.0.8.zip" },
    @{ Path = "apache/2.4.69/httpd-2.4.69-261002-Win64-VS18.zip"; Orig = "https://www.apachelounge.com/download/VS18/binaries/httpd-2.4.69-261002-Win64-VS18.zip" },
    # python-build-standalone names its files with "+"; on the CDN it is "-" (S3 may read "+" in a URL as a space).
    # Name = the file name in the official checksum list.
    @{ Path = "python/3.13.18/cpython-3.13.18-20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz";
       Name = "cpython-3.13.18+20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz";
       Sums = "https://github.com/astral-sh/python-build-standalone/releases/download/20261026/SHA256SUMS" },
    @{ Path = "python/3.12.16/cpython-3.12.16-20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz";
       Name = "cpython-3.12.16+20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz";
       Sums = "https://github.com/astral-sh/python-build-standalone/releases/download/20261026/SHA256SUMS" }
)

$temp = Join-Path ([IO.Path]::GetTempPath()) ("floserver-cdn-check-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $temp | Out-Null
$sumsCache = @{}
$results = @()

function Get-Text([string]$url) {
    $r = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 60
    if ($r.Content -is [byte[]]) { return [Text.Encoding]::UTF8.GetString($r.Content) }
    return [string]$r.Content
}

# The official checksum for $name from a checksum list: the line that names the
# file (as a whole word), and the 64-hex-digit value on that line.
function Find-OfficialHash([string]$sumsUrl, [string]$name) {
    if (-not $sumsCache.ContainsKey($sumsUrl)) {
        try { $sumsCache[$sumsUrl] = Get-Text $sumsUrl } catch { $sumsCache[$sumsUrl] = $null }
    }
    $text = $sumsCache[$sumsUrl]
    if ($null -eq $text) { return $null }
    $escaped = [regex]::Escape($name)
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match "(^|[^\w.-])$escaped([^\w.-]|$)") {
            if ($line -match "\b([0-9a-fA-F]{64})\b") { return $Matches[1].ToLower() }
        }
    }
    # composer.phar.sha256sum and similar hold just the hash, on one line
    $lines = @($text -split "`r?`n" | Where-Object { $_.Trim() -ne "" })
    if ($lines.Count -eq 1 -and $lines[0].Trim() -match "^([0-9a-fA-F]{64})(\s|$)") { return $Matches[1].ToLower() }
    return $null
}

foreach ($f in $files) {
    if ($Only -and -not $f.Path.Contains($Only)) { continue }
    $name = Split-Path $f.Path -Leaf
    $url = "$Cdn/$($f.Path)"
    $row = [ordered]@{ Path = $f.Path; Status = ""; Size = $null; Sha256 = ""; Official = ""; Via = ""; Result = ""; Code = "fail" }
    Write-Host "Checking $($f.Path) ..."
    $local = Join-Path $temp $name
    try {
        $resp = Invoke-WebRequest -Uri $url -UseBasicParsing -OutFile $local -PassThru -TimeoutSec 900
        $row.Status = [int]$resp.StatusCode
        $via = $resp.Headers["X-Cache"]
        if ($via -is [array]) { $via = $via[0] }
        $row.Via = [string]$via
    } catch {
        $code = $null
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        if ($code) { $row.Status = $code } else { $row.Status = "no answer" }
        $row.Result = "FAIL: the CDN doesn't serve this file"
        $results += New-Object PSObject -Property $row
        continue
    }
    $row.Size = (Get-Item $local).Length
    $row.Sha256 = (Get-FileHash -Algorithm SHA256 -Path $local).Hash.ToLower()

    $official = $null
    $source = ""
    if ($f.Sums) {
        $sumsName = $name
        if ($f.Name) { $sumsName = $f.Name }
        $official = Find-OfficialHash $f.Sums $sumsName
        $source = "checksum list"
    } elseif ($f.Orig) {
        $orig = Join-Path $temp ("original-" + $name)
        try {
            Invoke-WebRequest -Uri $f.Orig -UseBasicParsing -OutFile $orig -TimeoutSec 900
            $official = (Get-FileHash -Algorithm SHA256 -Path $orig).Hash.ToLower()
            $source = "original download"
            Remove-Item $orig -Force
        } catch { $official = $null }
    }

    if ($null -eq $official) {
        $row.Official = "not found"
        $row.Result = "CHECK BY HAND: no official checksum could be fetched"
        $row.Code = "unchecked"
    } elseif ($official -eq $row.Sha256) {
        $row.Official = "matches ($source)"
        $row.Result = "OK"
        $row.Code = "ok"
    } else {
        $row.Official = $official
        $row.Result = "FAIL: different from the official file"
    }
    Remove-Item $local -Force
    $results += New-Object PSObject -Property $row
}

# The bucket itself must refuse direct reads: only CloudFront may read it.
$direct = ""
if ($Bucket -and $Region) {
    # Any one listed file will do; the smallest keeps it quick.
    $probe = ($files | Where-Object { $_.Path -like "composer/*" } | Select-Object -First 1)
    if (-not $probe) { $probe = $files[0] }
    $s3 = "https://$Bucket.s3.$Region.amazonaws.com/$($probe.Path)"
    try {
        $r = Invoke-WebRequest -Uri $s3 -UseBasicParsing -Method Head -TimeoutSec 60
        $direct = "FAIL: $s3 answered $([int]$r.StatusCode); the bucket is readable without CloudFront"
    } catch {
        $code = $null
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        if ($code -eq 403) { $direct = "OK: direct S3 access is refused (403)" }
        else { $direct = "CHECK BY HAND: direct S3 access gave '$code' (expected 403)" }
    }
} else {
    $direct = "skipped (run with -Bucket <name> -Region <region> to check)"
}

Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue

$report = @()
$report += "FloServer CDN check, $(Get-Date -Format 'yyyy-MM-dd HH:mm') local time, $Cdn"
$report += ""
foreach ($r in $results) {
    $report += "$($r.Result)"
    $report += "  path:     $($r.Path)"
    $report += "  http:     $($r.Status)   via: $($r.Via)"
    $report += "  size:     $($r.Size)"
    $report += "  sha256:   $($r.Sha256)"
    $report += "  official: $($r.Official)"
    $report += ""
}
$report += "Direct S3 access: $direct"
$ok = @($results | Where-Object { $_.Result -eq "OK" }).Count
$report += "Summary: $ok of $($results.Count) files OK"

# Next to the script; the current folder when the script has no file (pasted, ISE).
$here = $PSScriptRoot
if (-not $here) { $here = (Get-Location).Path }
$out = Join-Path $here "floserver-cdn-report.txt"
$report | Set-Content -Path $out -Encoding UTF8

# Machine-readable copy for floserver-manifest build.
$jsonFiles = @()
foreach ($r in $results) {
    $jsonFiles += [ordered]@{ path = $r.Path; size = $r.Size; sha256 = $r.Sha256; result = $r.Code }
}
$checked = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ", [Globalization.CultureInfo]::InvariantCulture)
$json = [ordered]@{ cdn = $Cdn; only = $Only; checked = $checked; files = $jsonFiles } | ConvertTo-Json -Depth 4
# UTF-8 without a byte-order mark, which Windows PowerShell 5.1 would otherwise add.
$jsonOut = Join-Path $here "floserver-cdn-report.json"
[IO.File]::WriteAllText($jsonOut, $json, (New-Object Text.UTF8Encoding $false))
$report | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Reports saved to $out and $jsonOut"
