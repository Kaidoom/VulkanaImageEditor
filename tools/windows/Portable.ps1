#Requires -Version 5.1
# Assemble an unsigned portable test folder from a configured Release build.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$VcpkgRoot = 'C:\Dev\vcpkg',
    [string]$QtRoot = 'C:\Qt\6.10.3\msvc2022_64',
    [string]$VulkanRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$BuildRoot = 'C:\Dev\Vulkana',
    [ValidateRange(1, 32)][int]$Jobs = 6
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$destinationPath = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destinationPath) { throw 'Destination already exists. Choose a new folder; existing builds are never overwritten.' }
$source = (Resolve-Path "$PSScriptRoot/../..").Path
$build = Join-Path $BuildRoot 'release'
if (!(Test-Path "$build/CMakeCache.txt")) { throw 'Run Build.ps1 first to configure the Release build.' }
& "$PSScriptRoot/Setup.ps1" -VcpkgRoot $VcpkgRoot -QtRoot $QtRoot -VulkanRoot $VulkanRoot -BuildRoot $BuildRoot -Jobs $Jobs -SkipSmoke
function Invoke-PortableCommand([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Get-PortableRelativePath([string]$Root, [string]$Path) {
    # Every inventory entry is inside the staged bundle. Avoid GetRelativePath,
    # which is unavailable in Windows PowerShell's .NET Framework runtime.
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (!$fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Portable inventory path must be inside the staged bundle.'
    }
    return $fullPath.Substring($prefix.Length).Replace('\', '/')
}
# Revalidate/compile changed language catalogs as well as the application.
Invoke-PortableCommand cmake @('--build', $build, '--target', 'imageeditor', 'vulkana_language_packs', '--parallel', "$Jobs")
$cache = Get-Content "$build/CMakeCache.txt" -Raw -Encoding UTF8
if ($cache -notmatch 'IMAGEEDITOR_RELOCATABLE_BUNDLE:BOOL=ON' -or
    $cache -notmatch 'IMAGEEDITOR_DEVELOPMENT_RESOURCES:BOOL=OFF' -or
    $cache -notmatch 'CMAKE_BUILD_TYPE:STRING=Release') { throw 'Portable assembly requires the relocatable Release configuration from Build.ps1.' }

# Stage separately. Publish only after dependency and content checks pass.
$scratch = Join-Path $BuildRoot ('portable-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    Invoke-PortableCommand cmake @('--install', $build, '--prefix', $scratch)
    $bin = Join-Path $scratch 'bin'
    # No Qt Quick/OpenGL software rasterizer or runtime shader compiler is used
    # by this Vulkan + raster-Widgets application. Retain its image/PDF plugins,
    # SVG icons, native style, networking and Windows TLS backend.
    Invoke-PortableCommand "$QtRoot/bin/windeployqt.exe" @('--release', '--no-translations', '--no-compiler-runtime',
        '--no-opengl-sw', '--no-system-d3d-compiler', '--no-system-dxc-compiler',
        '--skip-plugin-types', 'generic', "$bin/imageeditor.exe")

    $crt = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC145.CRT'
    $runtimeRoots = @($crt, "$BuildRoot/installed/x64-vulkana-windows/bin", "$QtRoot/bin")
    $dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
    $pending = [Collections.Generic.Queue[string]]::new()
    $visited = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $imports = [Collections.Generic.List[object]]::new()
    Get-ChildItem $scratch -Recurse -File | Where-Object Extension -In '.exe', '.dll' |
        ForEach-Object { $pending.Enqueue($_.FullName) }
    while ($pending.Count) {
        $file = $pending.Dequeue()
        if (!$visited.Add($file)) { continue }
        $output = & $dumpbin /NOLOGO /DEPENDENTS $file
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports: $file" }
        foreach ($line in $output) {
            if ($line -notmatch '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { continue }
            $dll = $Matches[1]
            $target = Join-Path $bin $dll
            $origin = 'bundle'
            if (!(Test-Path -LiteralPath $target)) {
                $found = $null
                foreach ($root in $runtimeRoots) {
                    $candidate = Join-Path $root $dll
                    if (Test-Path -LiteralPath $candidate) { $found = $candidate; break }
                }
                if ($found) {
                    Copy-Item -LiteralPath $found -Destination $target
                    $pending.Enqueue($target)
                } elseif ($dll -match '^(api-ms-|ext-ms-)' -or (Test-Path -LiteralPath "$env:SystemRoot/System32/$dll")) {
                    $origin = 'Windows/driver'
                } else { throw "Unresolved dependency $dll required by $file" }
            }
            $imports.Add([ordered]@{ binary = Get-PortableRelativePath $scratch $file; dependency = $dll; origin = $origin })
        }
    }

    # Native/system dependencies are not copied from System32 or SDK folders.
    # Retain the precise upstream notices shipped with the reviewed libraries.
    $licenses = Join-Path $scratch 'share/licenses/vulkana-editor'
    foreach ($component in @('qpdf', 'libwebp', 'libjpeg-turbo', 'libspng', 'minizip-ng', 'zlib')) {
        $copyright = "$BuildRoot/installed/x64-vulkana-windows/share/$component/copyright"
        if (!(Test-Path $copyright)) { throw "Missing dependency notice: $component" }
        Copy-Item $copyright "$licenses/$component-copyright.txt"
    }
    $lock = Get-Content "$PSScriptRoot/dependencies/versions.json" -Raw -Encoding UTF8 | ConvertFrom-Json
    $qtInventory = Join-Path $licenses 'qt-inventory'
    New-Item -ItemType Directory -Path $qtInventory | Out-Null
    foreach ($module in @('qtbase', 'qtimageformats', 'qtpdf', 'qtsvg', 'qttranslations')) {
        Copy-Item "$QtRoot/sbom/$module-$($lock.qtVersion).spdx.json" $qtInventory
    }
    Copy-Item "$source/res/notices/licenses/LGPL-3.0.txt", "$source/res/notices/licenses/GPL-3.0.txt" $licenses
    $vsRoot = (Get-Item $env:VCToolsInstallDir).Parent.Parent.Parent.Parent.FullName
    Copy-Item "$vsRoot/Licenses/1033/Redist.txt" "$licenses/Microsoft-Redist.txt"
    Copy-Item "$source/LICENSE" "$scratch/LICENSE.txt"
    Copy-Item "$PSScriptRoot/PORTABLE-README.txt" "$scratch/README.txt"

    $forbidden = @(Get-ChildItem $scratch -Recurse -File | Where-Object {
        $_.Extension -in '.pdb', '.lib', '.obj', '.exp', '.py', '.ps1', '.so' -or
        $_.Name -match '^(Qt6Test|opengl32sw|dxcompiler|dxil|d3dcompiler_47)\.' -or
        $_.Name -match '^(vc_redist|imageeditor_.*tests)'
    })
    if ($forbidden.Count) { throw "Unexpected development payload: $($forbidden.FullName -join ', ')" }
    $inventory = @(Get-ChildItem $scratch -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = Get-PortableRelativePath $scratch $_.FullName;
            bytes = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $manifest = [ordered]@{ format = 1; purpose = 'Unsigned Windows portable test build'; files = $inventory; imports = $imports } |
        ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText("$scratch/portable-manifest.json", $manifest, [Text.UTF8Encoding]::new($false))
    $parent = Split-Path $destinationPath -Parent
    if (!(Test-Path -LiteralPath $parent)) { throw 'Destination parent must already exist.' }
    Move-Item -LiteralPath $scratch -Destination $destinationPath
    $files = @(Get-ChildItem $destinationPath -Recurse -File)
    $size = ($files | Measure-Object Length -Sum).Sum
    Write-Output "Portable folder: $destinationPath"
    Write-Output "Launch: $destinationPath\bin\imageeditor.exe"
    Write-Output ("Files: {0}; size: {1:N1} MiB" -f $files.Count, ($size / 1MB))
} catch {
    Write-Warning "Assembly was not published. Diagnostic files remain in $scratch"
    throw
}
