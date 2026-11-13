#Requires -Version 5.1
# Build an optional CUDA worker and stage its redistributable runtime closure.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$EngineDirectory,
    [Parameter(Mandatory)][string]$CudaRoot,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Destination,
    [string]$CudaManifest,
    [string]$Python = 'python',
    [ValidateRange(1, 32)][int]$Jobs = 4,
    [switch]$Tests
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or [IntPtr]::Size -ne 8) {
    throw 'Use an x64 Visual Studio developer PowerShell session on Windows.'
}
foreach ($program in @('cmake', 'ninja', 'cl.exe', 'dumpbin.exe', 'git', $Python)) {
    Get-Command $program -ErrorAction Stop | Out-Null
}
if (!$env:VCToolsRedistDir -or !$env:VCToolsInstallDir) {
    throw 'Initialize the x64 Visual Studio developer environment before building the worker.'
}
$source = (Resolve-Path "$PSScriptRoot/../..").Path
$engine = (Resolve-Path -LiteralPath $EngineDirectory).Path
$cuda = (Resolve-Path -LiteralPath $CudaRoot).Path
$build = [IO.Path]::GetFullPath($BuildDirectory)
$destinationPath = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destinationPath) { throw 'Destination already exists. Choose a new worker bundle folder.' }
if (!(Test-Path -LiteralPath (Split-Path $destinationPath -Parent) -PathType Container)) {
    throw 'Destination parent must already exist.'
}
if (!(Test-Path -LiteralPath "$cuda/bin/nvcc.exe")) { throw 'CudaRoot must contain bin/nvcc.exe.' }
$env:CUDA_PATH = $cuda
$env:PATH = "$cuda/bin;$env:PATH"
function Invoke-WorkerCommand([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
$testFlag = if ($Tests) { 'ON' } else { 'OFF' }
# Native cubins for RTX 30/40/50, plus generic Hopper PTX for forward JIT.
# Blackwell's architecture-specific PTX is not a forward-compatible fallback.
Invoke-WorkerCommand cmake @('-S', "$source/src/diffusion/worker", '-B', $build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', "-DVULKANA_DIFFUSION_ENGINE_DIR=$engine", '-DVULKANA_DIFFUSION_RELEASE=ON',
    '-DSD_CUDA=ON', '-DSD_VULKAN=OFF', '-DSD_BUILD_SHARED_LIBS=OFF', '-DGGML_BACKEND_DL=OFF',
    '-DGGML_NATIVE=OFF', '-DGGML_CPU_ALL_VARIANTS=OFF',
    '-DCMAKE_CUDA_ARCHITECTURES=86-real;89-real;120a-real;90-virtual',
    "-DCUDAToolkit_ROOT=$cuda", "-DCMAKE_CUDA_COMPILER=$cuda/bin/nvcc.exe", "-DBUILD_TESTING=$testFlag")
Invoke-WorkerCommand cmake @('--build', $build, '--parallel', "$Jobs")

$worker = @("$build/vulkana-diffusion-worker-cuda.exe", "$build/bin/vulkana-diffusion-worker-cuda.exe",
    "$build/vulkana-diffusion-worker.exe", "$build/bin/vulkana-diffusion-worker.exe") |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (!$worker) { throw 'The CUDA worker executable was not produced.' }
$crt = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC145.CRT'
$openmp = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC145.OpenMP'
$runtimeRoots = @("$cuda/bin/x64", "$cuda/bin", $crt, $openmp)
$vsRoot = (Get-Item $env:VCToolsInstallDir).Parent.Parent.Parent.Parent.FullName
$msvcLicense = Join-Path $vsRoot 'Licenses/1033/Redist.txt'
if (!(Test-Path -LiteralPath $msvcLicense)) { throw 'The Visual Studio redistributable license is missing.' }

$scratch = Join-Path (Split-Path $destinationPath -Parent) ('diffusion-worker-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$scratch/bin" | Out-Null
try {
    Copy-Item -LiteralPath $worker -Destination "$scratch/bin/vulkana-diffusion-worker-cuda.exe"
    $pending = [Collections.Generic.Queue[string]]::new()
    $visited = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $pending.Enqueue("$scratch/bin/vulkana-diffusion-worker-cuda.exe")
    while ($pending.Count) {
        $file = $pending.Dequeue()
        if (!$visited.Add($file)) { continue }
        $output = & dumpbin.exe /NOLOGO /DEPENDENTS $file
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports: $file" }
        foreach ($line in $output) {
            if ($line -notmatch '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { continue }
            $dll = $Matches[1]
            $target = Join-Path "$scratch/bin" $dll
            if (Test-Path -LiteralPath $target) { continue }
            $found = $null
            foreach ($root in $runtimeRoots) {
                $candidate = Join-Path $root $dll
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { $found = $candidate; break }
            }
            if ($found) {
                Copy-Item -LiteralPath $found -Destination $target
                $pending.Enqueue($target)
            } elseif ($dll -match '^(cublas|cudart|msvcp|vcruntime|vcomp|concrt)') {
                throw "Missing redistributable worker dependency in the selected SDKs: $dll"
            } elseif ($dll -match '^(api-ms-|ext-ms-)' -or
                (Test-Path -LiteralPath "$env:SystemRoot/System32/$dll" -PathType Leaf)) {
                Write-Output "Using Windows/driver-provided $dll"
            } else { throw "Unresolved worker dependency $dll required by $file" }
        }
    }
    $arguments = @("$source/tools/diffusion/prepare_bundle.py", '--engine', $engine, '--build', $build,
        '--prefix', $scratch, '--cuda-root', $cuda, '--platform', 'windows', '--msvc-license', $msvcLicense)
    if ($CudaManifest) { $arguments += @('--cuda-manifest', (Resolve-Path -LiteralPath $CudaManifest).Path) }
    Invoke-WorkerCommand $Python $arguments
    Invoke-WorkerCommand $Python @("$source/tools/diffusion/prepare_bundle.py", '--verify', $scratch)
    if ($Tests) {
        # Helpers run beside the build executable; inference remains a separate
        # opt-in check using the staged worker and user-provided model files.
        foreach ($dll in Get-ChildItem "$scratch/bin/*.dll") {
            Copy-Item -LiteralPath $dll.FullName -Destination (Split-Path $worker -Parent) -Force
        }
        Invoke-WorkerCommand ctest @('--test-dir', $build, '--output-on-failure', '--parallel', '1')
    }
    [IO.Directory]::Move($scratch, $destinationPath)
    Write-Output "Diffusion worker bundle: $destinationPath"
} catch {
    Write-Warning "Worker bundle was not published. Diagnostic files remain in $scratch"
    throw
}
