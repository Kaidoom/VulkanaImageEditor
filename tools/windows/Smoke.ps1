# Run from an interactive desktop. Only the isolated smoke-test process is closed.
[CmdletBinding()]
param(
    [string]$BuildRoot = 'C:\Dev\Vulkana',
    [string]$Executable = '',
    [ValidateRange(10, 180)][int]$TimeoutSeconds = 90,
    [switch]$Relocate
)
$ErrorActionPreference = 'Stop'
if (!$Executable) { $Executable = Join-Path $BuildRoot 'stage/bin/imageeditor.exe' }
$logs = Join-Path $BuildRoot 'validation'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$relocationRoot = ''
if ($Relocate) {
    # A unique path with spaces and Unicode checks DLL and data-path discovery.
    $bundle = Split-Path (Split-Path $Executable -Parent) -Parent
    if (!(Test-Path "$bundle/share/vulkana-editor")) { throw 'Expected the complete staged bundle, not a bare executable.' }
    $relocationRoot = Join-Path $logs ('portable café-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $relocationRoot | Out-Null
    Get-ChildItem -LiteralPath $bundle | Copy-Item -Destination $relocationRoot -Recurse
    $Executable = Join-Path $relocationRoot 'bin/imageeditor.exe'
}
# Verify app-local dependencies, not incidental Qt/vcpkg directories in PATH.
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
$env:QT_FORCE_STDERR_LOGGING = '1'
$stdout = Join-Path $logs 'smoke.stdout.log'
$stderr = Join-Path $logs 'smoke.stderr.log'
$process = Start-Process -FilePath $Executable -ArgumentList @('--smoke-test', '--language', 'en', '-platform', 'windows') -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
    $process.Kill()
    $process.WaitForExit()
    throw "Native smoke test exceeded its deadline. See $logs"
}
$process.WaitForExit()
$log = Get-Content -LiteralPath $stderr -Raw
Write-Output $log
if ($process.ExitCode -ne 0) { throw "Native smoke test failed with exit code $($process.ExitCode)." }
if ($log -notmatch 'Vulkan validation summary: warnings 0 errors 0') {
    throw 'Native smoke did not report a clean validation completion.'
}
Write-Output 'Native Windows Vulkan lifecycle smoke passed.'
if ($relocationRoot) {
    # Remove only this invocation's successful test copy; failures remain inspectable.
    Remove-Item -LiteralPath $relocationRoot -Recurse -Force
    Write-Output 'Portable relocation with spaces/Unicode and a clean PATH passed.'
}
