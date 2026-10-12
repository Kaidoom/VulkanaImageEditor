#Requires -Version 5.1
# Standalone checks; no SDK, Pester module or application launch is needed.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$sourceRoot = (Resolve-Path "$PSScriptRoot/..").Path
$scripts = Get-ChildItem "$sourceRoot/tools/windows/*.ps1"
foreach ($script in $scripts) {
    $tokens = $null
    $errors = $null
    $content = Get-Content -LiteralPath $script.FullName -Raw -Encoding UTF8
    [void][Management.Automation.Language.Parser]::ParseInput($content, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "Cannot parse $($script.Name): $errors" }
    if ($content -match '[^\x00-\x7F]') { throw "BOM-less script must stay ASCII for Windows PowerShell: $($script.Name)" }
}

# Load only the pure path helper, never the packaging script's top-level work.
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    "$sourceRoot/tools/windows/Portable.ps1", [ref]$tokens, [ref]$errors)
$helper = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-PortableRelativePath'
}, $true)
if (!$helper) { throw 'Missing portable path helper.' }
. ([scriptblock]::Create($helper.Extent.Text))

function Assert-RelativePath([string]$Root, [string]$Path, [string]$Expected) {
    $actual = Get-PortableRelativePath $Root $Path
    if ($actual -cne $Expected) { throw "Relative path mismatch: $actual != $Expected" }
}
Assert-RelativePath 'C:\portable' 'C:\portable\bin\app.exe' 'bin/app.exe'
Assert-RelativePath 'C:\portable\' 'c:\PORTABLE\share\a.json' 'share/a.json'
Assert-RelativePath 'C:\portable' 'C:\portable\bin\..\share\a.json' 'share/a.json'
Assert-RelativePath '\\server\share\portable' '\\server\share\portable\bin\app.exe' 'bin/app.exe'
$unicodeRoot = 'C:\portable caf' + [char]0xE9
$unicodeFile = [char]0x82B1 + '.json'
Assert-RelativePath $unicodeRoot "$unicodeRoot\share\$unicodeFile" "share/$unicodeFile"
foreach ($outside in @('C:\portable-sibling\app.exe', 'C:\portable\..\app.exe', 'D:\portable\app.exe')) {
    $rejected = $false
    try { [void](Get-PortableRelativePath 'C:\portable' $outside) } catch { $rejected = $true }
    if (!$rejected) { throw "Inventory accepted an outside path: $outside" }
}

# Match the smoke wrappers' redirected process + timed wait, including failures.
$work = Join-Path ([IO.Path]::GetTempPath()) ('vulkana-script-check-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    foreach ($code in @(0, 7)) {
        $process = Start-Process "$env:SystemRoot/System32/cmd.exe" -ArgumentList "/c exit $code" -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput "$work/stdout.log" -RedirectStandardError "$work/stderr.log"
        $null = $process.Handle
        if (!$process.WaitForExit(10000)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'Process exit-code check timed out.'
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne $code) { throw "Lost process exit code: expected $code, got $($process.ExitCode)." }
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
Write-Output "Windows build script checks passed on PowerShell $($PSVersionTable.PSVersion)."
