param([switch]$Test)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'The Visual C++ build tools were not found.' }
$vsVersion = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$generator = if ($vsVersion.StartsWith('16.')) { 'Visual Studio 16 2019' } else { 'Visual Studio 17 2022' }
cmake -S $projectRoot -B (Join-Path $projectRoot 'build') -G $generator -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build (Join-Path $projectRoot 'build') --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Test) {
    ctest --test-dir (Join-Path $projectRoot 'build') -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
$distribution = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $distribution -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'build\release\Timelapse.exe') -Destination $distribution
if (Test-Path -LiteralPath (Join-Path $projectRoot 'README.md')) { Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $distribution }
$packageFiles = @((Join-Path $distribution 'Timelapse.exe'))
if (Test-Path -LiteralPath (Join-Path $distribution 'README.md')) { $packageFiles += (Join-Path $distribution 'README.md') }
Compress-Archive -LiteralPath $packageFiles -DestinationPath (Join-Path $distribution 'Timelapse-portable.zip') -Force
$hashes = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $distribution 'Timelapse.exe'),(Join-Path $distribution 'Timelapse-portable.zip')
$hashes | ForEach-Object { $_.Hash.ToLowerInvariant() + '  ' + (Split-Path -Leaf $_.Path) } | Set-Content -LiteralPath (Join-Path $distribution 'SHA256SUMS.txt') -Encoding ascii
Get-Item -LiteralPath (Join-Path $distribution 'Timelapse.exe') | Select-Object FullName,Length
