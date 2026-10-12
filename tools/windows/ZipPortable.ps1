#Requires -Version 5.1
# Validate and archive a prepared portable folder; never launch its contents.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PortableDirectory,
    [Parameter(Mandatory)][string]$Destination
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path -LiteralPath $PortableDirectory).Path.TrimEnd([char[]]'\/')
$target = [IO.Path]::GetFullPath($Destination)
if ([IO.Path]::GetExtension($target) -ine '.zip') { throw 'Destination must end in .zip.' }
if (Test-Path -LiteralPath $target) { throw 'Destination already exists; choose a new ZIP path.' }
if ($target.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'ZIP destination must be outside the portable folder.'
}
if (!(Test-Path -LiteralPath (Split-Path $target -Parent))) { throw 'Destination parent must exist.' }
$notices = Get-Content "$root/share/vulkana-editor/notices/notices.json" -Raw -Encoding UTF8 | ConvertFrom-Json
if ($notices.profile -ne 'windowsportable') { throw 'Prepare matching distribution notices before creating a release ZIP.' }
foreach ($name in @('THIRD-PARTY-NOTICES.txt', 'SOURCES.json', 'SOURCE-SHA256SUMS', 'LIBRARY-REPLACEMENT.txt')) {
    if (!(Test-Path "$root/share/vulkana-editor/notices/$name" -PathType Leaf)) { throw "Missing release notice: $name" }
}
$manifest = Get-Content "$root/portable-manifest.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$expected = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in $manifest.files) {
    if ($file.path -match '(^/|\\|:|(^|/)\.\.?(/|$))' -or $file.sha256 -notmatch '^[a-f0-9]{64}$') {
        throw 'Invalid portable inventory entry.'
    }
    $expected.Add($file.path, $file.sha256)
}
$expected.Add('portable-manifest.json', (Get-FileHash "$root/portable-manifest.json" -Algorithm SHA256).Hash)
$files = @(Get-ChildItem -LiteralPath $root -Recurse -File)
if ($files.Count -ne $expected.Count) { throw 'Portable folder contains missing or unlisted files.' }
foreach ($file in $files) {
    $relative = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
    if (!$expected.ContainsKey($relative) -or (Get-FileHash $file.FullName -Algorithm SHA256).Hash -ine $expected[$relative]) {
        throw "Portable file differs from its inventory: $relative"
    }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$temporary = $target + '.' + [guid]::NewGuid().ToString('N') + '.partial'
try {
    # .NET Framework's directory helper can emit backslash entry names. Write
    # canonical ZIP paths explicitly for Explorer and cross-platform readers.
    $archive = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $prefix = (Split-Path $root -Leaf) + '/'
        foreach ($file in ($files | Sort-Object FullName)) {
            $relative = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $file.FullName, $prefix + $relative, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    $archive = [IO.Compression.ZipFile]::OpenRead($temporary)
    try {
        $prefix = (Split-Path $root -Leaf) + '/'
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            $entryPath = $entry.FullName
            if ($entryPath.Contains('\')) { throw 'ZIP entries must use forward slashes.' }
            if (!$entryPath.StartsWith($prefix, [StringComparison]::Ordinal)) { throw 'Unexpected archive root.' }
            if ($entryPath.EndsWith('/')) { continue }
            $relative = $entryPath.Substring($prefix.Length)
            if (!$expected.ContainsKey($relative) -or !$seen.Add($relative)) { throw 'Unexpected/duplicate archive member.' }
            $stream = $entry.Open()
            $hasher = [Security.Cryptography.SHA256]::Create()
            try { $hash = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
            finally { $hasher.Dispose(); $stream.Dispose() }
            if ($hash -ine $expected[$relative]) { throw "ZIP verification failed: $relative" }
        }
        if ($seen.Count -ne $expected.Count) { throw 'ZIP omitted portable files.' }
    } finally { $archive.Dispose() }
    Move-Item -LiteralPath $temporary -Destination $target
    Write-Output "ZIP: $target"
    Write-Output ("SHA256: " + (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant())
    Write-Output ("Bytes: " + (Get-Item -LiteralPath $target).Length)
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
}
