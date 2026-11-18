# make-postgresql-server-zip.ps1  (D29)
# Makes the PostgreSQL zip FloServer installs from EDB's Windows binaries zip:
# the same files at the same paths, without pgAdmin 4, StackBuilder, the docs
# and the debug symbols (EDB's zip is about 350 MB, most of it pgAdmin).
#
#   postgresql-18.1-1-windows-x64-binaries.zip  ->  postgresql-18.1-1-windows-x64-server.zip
#
# Nothing is unpacked: each kept file is copied from one zip to the other, so
# Windows' path length limit (pgAdmin's deep folders) doesn't matter.
# FloServer Publisher then checks every file in the new zip against EDB's own
# zip, byte for byte, before the runtime list can name it.
#
# Run in PowerShell, with EDB's zip already downloaded:
#   powershell -ExecutionPolicy Bypass -File .\make-postgresql-server-zip.ps1 -Zip .\postgresql-18.1-1-windows-x64-binaries.zip
# or let it download EDB's zip:
#   powershell -ExecutionPolicy Bypass -File .\make-postgresql-server-zip.ps1 -Version 18.1-1
#
# Upload the result to the CDN as postgresql/<version>/<file>, for example
#   postgresql/18.1/postgresql-18.1-1-windows-x64-server.zip

param(
    [string]$Zip = "",       # EDB's postgresql-<v>-<n>-windows-x64-binaries.zip
    [string]$Version = "",   # or its version and build, e.g. 18.1-1, to download it
    [string]$OutDir = ""     # where the server zip goes; default: next to EDB's zip
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is very slow with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Kept: the folders the server and its tools need. Everything else in EDB's zip
# (pgAdmin 4, StackBuilder, doc, symbols, ...) is left out.
$keep = @("pgsql/bin/", "pgsql/lib/", "pgsql/share/", "pgsql/include/")
# Must be there, as the Publisher also checks.
$required = @("pgsql/bin/postgres.exe", "pgsql/bin/initdb.exe", "pgsql/bin/pg_ctl.exe", "pgsql/bin/psql.exe",
              "pgsql/bin/pg_dump.exe", "pgsql/bin/libpq.dll", "pgsql/share/postgres.bki")

if ($Zip -eq "" -and $Version -eq "") {
    throw "Give -Zip <EDB's binaries zip> or -Version <version-build>, e.g. -Version 18.1-1"
}
if ($Zip -eq "") {
    if ($Version -notmatch '^\d+\.\d+-\d+$') { throw "-Version is <version>-<build>, e.g. 18.1-1" }
    $name = "postgresql-$Version-windows-x64-binaries.zip"
    $Zip = Join-Path (Get-Location) $name
    if (-not (Test-Path $Zip)) {
        Write-Host "Downloading $name from EDB (about 350 MB)..."
        Invoke-WebRequest -Uri "https://get.enterprisedb.com/postgresql/$name" -OutFile $Zip -UseBasicParsing
    }
}
$Zip = (Resolve-Path $Zip).Path
$source = Split-Path $Zip -Leaf
if ($source -notmatch '^postgresql-(\d+\.\d+)-(\d+)-windows-x64-binaries\.zip$') {
    throw "$source isn't named like EDB's zip (postgresql-<version>-<build>-windows-x64-binaries.zip)"
}
$pgVersion = $Matches[1]
$outName = $source -replace '-binaries\.zip$', '-server.zip'
if ($OutDir -eq "") { $OutDir = Split-Path $Zip -Parent }
$out = Join-Path $OutDir $outName
if (Test-Path $out) { Remove-Item $out }

$in = [System.IO.Compression.ZipFile]::OpenRead($Zip)
$dest = [System.IO.Compression.ZipFile]::Open($out, [System.IO.Compression.ZipArchiveMode]::Create)
$kept = 0
$left = 0
$names = @{}
try {
    foreach ($e in $in.Entries) {
        $path = $e.FullName -replace '\\', '/'
        if ($path.EndsWith("/")) { continue }   # folders come with their files
        $wanted = $false
        foreach ($k in $keep) { if ($path.StartsWith($k)) { $wanted = $true; break } }
        if (-not $wanted) { $left++; continue }
        # The same name as in EDB's zip, so the Publisher finds each file at the same path.
        $copy = $dest.CreateEntry($e.FullName, [System.IO.Compression.CompressionLevel]::Optimal)
        $copy.LastWriteTime = $e.LastWriteTime
        $from = $e.Open()
        $to = $copy.Open()
        try { $from.CopyTo($to) } finally { $to.Dispose(); $from.Dispose() }
        $names[$path] = $true
        $kept++
    }
} finally {
    $dest.Dispose()
    $in.Dispose()
}

$missing = @($required | Where-Object { -not $names.ContainsKey($_) })
if ($missing.Count -gt 0) {
    Remove-Item $out
    throw "EDB's zip has no " + ($missing -join ", ") + "; is it the Windows x64 binaries zip?"
}

$size = [math]::Round((Get-Item $out).Length / 1MB, 1)
$hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
Write-Host ""
Write-Host "Made $out"
Write-Host "  $kept files kept, $left left out (pgAdmin 4, StackBuilder, docs, symbols), $size MB"
Write-Host "  SHA-256 $hash"
Write-Host ""
Write-Host "Upload it to the CDN as postgresql/$pgVersion/$outName, then add it in FloServer Publisher."
