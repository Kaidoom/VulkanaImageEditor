#Requires -Version 5.1
# Verify the assembled folder on an interactive desktop without changing it.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PortableDirectory,
    [string]$BuildRoot = 'C:\Dev\Vulkana'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$bundle = (Resolve-Path -LiteralPath $PortableDirectory).Path
New-Item -ItemType Directory -Path "$BuildRoot/validation" -Force | Out-Null
Start-Transcript -Path "$BuildRoot/validation/portable-review.log" -Force
$manifest = Get-Content "$bundle/portable-manifest.json" -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($file in $manifest.files) {
    $path = Join-Path $bundle $file.path
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path).Hash -ne $file.sha256) {
        throw "Portable inventory mismatch: $($file.path)"
    }
}
& "$PSScriptRoot/Smoke.ps1" -BuildRoot $BuildRoot -Executable "$bundle/bin/imageeditor.exe" -Relocate

# Test dynamically loaded paths as well as the ordinary PE import closure.
# Only probe executables are added to an isolated copy, never Qt/SDK DLLs.
$probe = Join-Path $BuildRoot ('validation/portable-probes-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $probe | Out-Null
Get-ChildItem -LiteralPath $bundle | Copy-Item -Destination $probe -Recurse
try {
    foreach ($name in @('windows_dependency_probe.exe', 'windows_object_runtime_probe.exe')) {
        Copy-Item "$BuildRoot/dependency-smoke/$name" "$probe/bin"
    }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'windows'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
    & "$probe/bin/windows_dependency_probe.exe" "$probe/share/vulkana-editor/shaders/composite.comp.spv"
    if ($LASTEXITCODE -ne 0) { throw 'Portable image/PDF/SVG/TLS/Vulkan dependency probe failed.' }
    & "$probe/bin/windows_object_runtime_probe.exe" "$probe/share/vulkana-editor/object-selection/mobilesam-v1"
    if ($LASTEXITCODE -ne 0) { throw 'Portable offline MobileSAM encoder/decoder probe failed.' }
    Write-Output 'Portable inventory, relocation, canvas, image/PDF/SVG/TLS and offline model checks passed.'
} finally {
    Remove-Item -LiteralPath $probe -Recurse -Force
    Stop-Transcript
}
