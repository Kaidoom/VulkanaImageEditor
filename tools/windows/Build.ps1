#Requires -Version 5.1
# Build and stage an x64 developer/test application; this does not publish a release.
[CmdletBinding()]
param(
    [string]$VcpkgRoot = 'C:\Dev\vcpkg',
    [string]$QtRoot = 'C:\Qt\6.10.3\msvc2022_64',
    [string]$VulkanRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$BuildRoot = 'C:\Dev\Vulkana',
    [ValidateRange(1, 32)][int]$Jobs = 6,
    [switch]$Tests
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
& "$PSScriptRoot/Setup.ps1" -VcpkgRoot $VcpkgRoot -QtRoot $QtRoot -VulkanRoot $VulkanRoot -BuildRoot $BuildRoot -Jobs $Jobs -SkipSmoke
function Invoke-BuildCommand([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
$source = (Resolve-Path "$PSScriptRoot/../..").Path
$build = Join-Path $BuildRoot 'release'
$stage = Join-Path $BuildRoot 'stage'
$lock = Get-Content "$PSScriptRoot/dependencies/versions.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$runtime = Join-Path $BuildRoot ('runtime/onnxruntime-win-x64-' + $lock.onnxRuntime.version)
$testFlag = if ($Tests) { 'ON' } else { 'OFF' }
Invoke-BuildCommand cmake @('-S', $source, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$build/bin",
    "-DCMAKE_PREFIX_PATH=$QtRoot", "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
    '-DVCPKG_MANIFEST_MODE=OFF', "-DVCPKG_INSTALLED_DIR=$BuildRoot/installed", '-DVCPKG_TARGET_TRIPLET=x64-vulkana-windows',
    "-DVULKANA_WINDOWS_ONNX_ROOT=$runtime", "-DCMAKE_INSTALL_PREFIX=$stage",
    '-DIMAGEEDITOR_DEVELOPMENT_RESOURCES=OFF', '-DIMAGEEDITOR_RELOCATABLE_BUNDLE=ON',
    "-DIMAGEEDITOR_BUILD_TESTS=$testFlag")
Invoke-BuildCommand cmake @('--build', $build, '--parallel', "$Jobs")
Invoke-BuildCommand cmake @('--install', $build)
# vcpkg's app-local deployment resolves its own transitive native DLL closure.
foreach ($dll in Get-ChildItem "$build/bin/*.dll") {
    Copy-Item -LiteralPath $dll.FullName -Destination "$stage/bin" -Force
}
Invoke-BuildCommand "$QtRoot/bin/windeployqt.exe" @('--release', '--no-translations', '--no-compiler-runtime', "$stage/bin/imageeditor.exe")
# App-local redistributable CRT; a portable build must not require running an
# installer or finding a development toolchain on the recipient's PATH.
$crt = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC145.CRT'
if (!(Test-Path "$crt/vcruntime140.dll")) { throw "Missing redistributable x64 CRT: $crt" }
Copy-Item "$crt/*.dll" -Destination "$stage/bin" -Force
if ($Tests) {
    # Relocatable tests need the installed data beside their own executables,
    # including offline models, translations and notices on a fresh checkout.
    Invoke-BuildCommand cmake @('--install', $build, '--prefix', $build)
    Invoke-BuildCommand "$QtRoot/bin/windeployqt.exe" @('--release', '--no-translations', '--no-compiler-runtime', "$build/bin/imageeditor.exe")
    Copy-Item "$QtRoot/bin/Qt6Test.dll" -Destination "$build/bin" -Force
    Copy-Item "$crt/*.dll" -Destination "$build/bin" -Force
    # Native clipboard, drag and desktop capture tests share the user session.
    Invoke-BuildCommand ctest @('--test-dir', $build, '--output-on-failure', '--parallel', '1')
}
Write-Output "Staged executable: $stage\bin\imageeditor.exe"
Write-Output 'Preferences remain in per-user storage, not in this application directory.'
