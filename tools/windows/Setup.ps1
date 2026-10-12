# Developer setup only. No machine-wide PATH changes, driver installs, Python
# packages, model re-export, app packaging or changes to the Linux build.
[CmdletBinding()]
param(
    [string]$VcpkgRoot = 'C:\Dev\vcpkg',
    [string]$QtRoot = 'C:\Qt\6.10.3\msvc2022_64',
    [string]$VulkanRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$BuildRoot = 'C:\Dev\Vulkana',
    [ValidateRange(1, 32)][int]$Jobs = 6,
    [switch]$SkipSmoke
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$IsWindows) { throw 'Run this setup in PowerShell 7 on Windows.' }
$PSStyle.OutputRendering = 'PlainText'
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lock = Get-Content (Join-Path $PSScriptRoot 'dependencies/versions.json') -Raw | ConvertFrom-Json

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

foreach ($path in @("$VcpkgRoot/vcpkg.exe", "$QtRoot/bin/qmake.exe", "$QtRoot/bin/lrelease.exe",
        "$VulkanRoot/Bin/glslc.exe", "$VulkanRoot/Include/vulkan/vulkan.h", "$VulkanRoot/Lib/vulkan-1.lib")) {
    if (!(Test-Path -LiteralPath $path)) { throw "Missing prerequisite: $path" }
}
$commit = & git -C $VcpkgRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $commit -ne $lock.vcpkgCommit) { throw 'vcpkg checkout differs from the reviewed dependency baseline.' }
$dirty = & git -C $VcpkgRoot status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'vcpkg has tracked modifications; use an unmodified pinned checkout.' }
$qtVersion = & "$QtRoot/bin/qmake.exe" -query QT_VERSION
if ($LASTEXITCODE -ne 0 -or $qtVersion -ne $lock.qtVersion) { throw 'Qt version differs from the Windows setup lock.' }
if ((Split-Path $VulkanRoot -Leaf) -ne $lock.vulkanSdkVersion) { throw 'Vulkan SDK version differs from the Windows setup lock.' }
foreach ($program in @('cmake', 'ninja', 'python', 'git')) { Get-Command $program -ErrorAction Stop | Out-Null }
Invoke-Checked python @('--version')

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$vsRoot) { throw 'Visual Studio x64 C++ tools are required.' }
& (Join-Path $vsRoot 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
if ($env:VCToolsVersion.TrimEnd('\', '/') -ne $lock.msvcToolsVersion) { throw 'MSVC toolset differs from the Windows setup lock.' }
# These environment settings are scoped to this PowerShell process and children.
$env:VCPKG_ROOT = $VcpkgRoot
$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_MAX_CONCURRENCY = "$Jobs"
$env:VULKAN_SDK = $VulkanRoot
$env:PATH = "$QtRoot/bin;$VulkanRoot/Bin;$env:PATH"
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$BuildRoot = (Resolve-Path $BuildRoot).Path
$installed = Join-Path $BuildRoot 'installed'
$triplets = Join-Path $PSScriptRoot 'triplets'
Invoke-Checked "$VcpkgRoot/vcpkg.exe" @('install', '--disable-metrics',
    "--x-manifest-root=$PSScriptRoot/dependencies", "--x-install-root=$installed",
    "--overlay-triplets=$triplets", '--triplet=x64-vulkana-windows', '--host-triplet=x64-windows')

$downloads = Join-Path $BuildRoot 'downloads'
New-Item -ItemType Directory -Force -Path $downloads | Out-Null
$archive = Join-Path $downloads $lock.onnxRuntime.archive
if (!(Test-Path -LiteralPath $archive)) {
    # Unique pending file; failure never overwrites a previously verified archive.
    $pending = Join-Path $downloads ($lock.onnxRuntime.archive + '.' + [guid]::NewGuid().ToString('N') + '.partial')
    try {
        Invoke-WebRequest -Uri $lock.onnxRuntime.url -OutFile $pending
        if ((Get-FileHash $pending -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.onnxRuntime.sha256) {
            throw 'ONNX Runtime archive checksum mismatch.'
        }
        Move-Item -LiteralPath $pending -Destination $archive
    } finally {
        if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending }
    }
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.onnxRuntime.sha256) {
    throw "Cached archive checksum mismatch; inspect $archive before retrying."
}
$runtimeParent = Join-Path $BuildRoot 'runtime'
$runtime = Join-Path $runtimeParent ('onnxruntime-win-x64-' + $lock.onnxRuntime.version)
if (!(Test-Path -LiteralPath $runtime)) {
    # Extract once from the verified archive, retaining all official licenses.
    New-Item -ItemType Directory -Force -Path $runtimeParent | Out-Null
    $staging = Join-Path $runtimeParent ('extract-' + [guid]::NewGuid().ToString('N'))
    try {
        Expand-Archive -LiteralPath $archive -DestinationPath $staging
        $extracted = Join-Path $staging (Split-Path $runtime -Leaf)
        if ((Get-FileHash (Join-Path $extracted 'lib/onnxruntime.dll') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.onnxRuntime.dllSha256) {
            throw 'Extracted ONNX Runtime DLL checksum mismatch.'
        }
        Move-Item -LiteralPath $extracted -Destination $runtime
    } finally {
        # Only this invocation's uniquely named extraction directory is removed.
        if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    }
}
foreach ($name in @('lib/onnxruntime.dll', 'LICENSE', 'ThirdPartyNotices.txt')) {
    if (!(Test-Path -LiteralPath (Join-Path $runtime $name))) { throw "Incomplete ONNX Runtime extraction: $name" }
}
if ((Get-FileHash (Join-Path $runtime 'lib/onnxruntime.dll') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.onnxRuntime.dllSha256) {
    throw 'Cached ONNX Runtime DLL differs from the verified archive.'
}

if (!$SkipSmoke) {
    $smoke = Join-Path $BuildRoot 'dependency-smoke'
    Invoke-Checked cmake @('-S', "$PSScriptRoot/smoke", '-B', $smoke, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$QtRoot",
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", '-DVCPKG_MANIFEST_MODE=OFF',
        "-DVCPKG_INSTALLED_DIR=$installed", '-DVCPKG_TARGET_TRIPLET=x64-vulkana-windows',
        "-DVULKANA_SOURCE_ROOT=$sourceRoot", "-DONNX_RUNTIME_ROOT=$runtime")
    Invoke-Checked cmake @('--build', $smoke, '--parallel', "$Jobs")
    Invoke-Checked ctest @('--test-dir', $smoke, '--output-on-failure')
}
Write-Output "Dependencies: $installed"
Write-Output "Verified ONNX Runtime SDK: $runtime"
Write-Output 'Windows prerequisite setup complete. Build.ps1 builds and stages the application.'
