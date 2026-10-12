#Requires -Version 5.1
# Run on an interactive Windows desktop after Build.ps1 -Tests.
[CmdletBinding()]
param(
    [string]$BuildRoot = 'C:\Dev\Vulkana',
    [string]$QtRoot = 'C:\Qt\6.10.3\msvc2022_64',
    [string]$CaptureName = 'overlay'
)
$ErrorActionPreference = 'Stop'
$logs = Join-Path $BuildRoot ('validation/' + $CaptureName)
New-Item -ItemType Directory -Path $logs -Force | Out-Null
# Keep test binaries out of the portable bundle. An isolated copy also allows
# the editor to stay open while the test uses its own settings and fixture.
$testRoot = Join-Path $BuildRoot ('validation/overlay-run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
Get-ChildItem "$BuildRoot/stage" | Copy-Item -Destination $testRoot -Recurse
Copy-Item "$BuildRoot/release/bin/imageeditor_windows_overlay_tests.exe" "$testRoot/bin"
Copy-Item "$QtRoot/bin/Qt6Test.dll" "$testRoot/bin"
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:VULKANA_OVERLAY_CAPTURE_DIR = $logs
try {
    $process = Start-Process "$testRoot/bin/imageeditor_windows_overlay_tests.exe" -PassThru `
        -RedirectStandardOutput "$logs/stdout.log" -RedirectStandardError "$logs/stderr.log"
    # Keep ExitCode available after the timed wait on Windows PowerShell 5.1.
    $null = $process.Handle
    if (!$process.WaitForExit(60000)) {
        $process.Kill()
        $process.WaitForExit()
        throw 'Overlay pixel test exceeded its deadline.'
    }
    $process.WaitForExit()
    Get-Content "$logs/stdout.log"
    Get-Content "$logs/stderr.log"
    if ($process.ExitCode -ne 0) { throw "Overlay pixel test failed: $($process.ExitCode). See $logs" }
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
