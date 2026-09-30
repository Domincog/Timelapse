param(
    [Parameter(Mandatory = $true)][string]$Compiler,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$PayloadDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^(\d+\.\d+\.\d+)([-.][A-Za-z0-9.-]+)?$') { throw 'Invalid installer version.' }
$numericVersion = $Matches[1] + '.0'
$Compiler = [System.IO.Path]::GetFullPath($Compiler)
$payload = [System.IO.Path]::GetFullPath($PayloadDirectory)
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) { throw 'Supply the path to Inno Setup 7 ISCC.exe.' }
$compilerVersion = & $Compiler '--version'
if ($LASTEXITCODE -ne 0 -or ($compilerVersion -join ' ') -notmatch '\b([7-9]|[1-9][0-9]+)\.\d+\.\d+\b') { throw 'Inno Setup 7 or newer is required.' }
foreach ($name in @('Timelapse.exe', 'README.md', 'SHA256SUMS.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $name) -PathType Leaf)) { throw "Missing frozen installer payload: $name" }
}
# Reject an accidental ARM/x86 or non-PE payload before making a mislabeled setup.
$stream = [System.IO.File]::OpenRead((Join-Path $payload 'Timelapse.exe'))
$reader = New-Object System.IO.BinaryReader($stream)
try {
    if ($reader.ReadUInt16() -ne 0x5a4d -or $stream.Length -lt 64) { throw 'Installer requires a Windows x64 executable.' }
    $stream.Position = 60; $header = $reader.ReadUInt32()
    if ($header -gt $stream.Length - 6) { throw 'Invalid executable PE header.' }
    $stream.Position = $header
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Installer requires a Windows x64 executable.' }
} finally { $reader.Dispose(); $stream.Dispose() }
[System.IO.Directory]::CreateDirectory($output) | Out-Null
$destination = Join-Path $output "Timelapse-v$Version-windows-x64-setup.exe"
if (Test-Path -LiteralPath $destination) { throw "Choose an unused installer output: $destination" }
& $Compiler '/Qp' "/DAppVersion=$Version" "/DNumericVersion=$numericVersion" "/DPayloadDirectory=$payload" "/DPackageOutput=$output" (Join-Path $PSScriptRoot 'Timelapse.iss')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $destination -PathType Leaf)) { throw 'Inno Setup compilation failed.' }
Get-Item -LiteralPath $destination
