param([string]$Version = '0.1.0', [string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][A-Za-z0-9.-]+)?$') { throw 'Use a version such as 0.1.0 or 0.1.0-beta.1.' }
$projectRoot = $PSScriptRoot
$executable = Join-Path (Join-Path $projectRoot $BuildDirectory) 'release\Timelapse.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Build and test Release first: .\build.ps1 -Test' }
$packages = Join-Path $projectRoot 'packages'
New-Item -ItemType Directory -Path $packages -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Write-Package([string]$Destination, [object[]]$Files, [hashtable]$TextFiles = @{}) {
    $stream = [System.IO.File]::Open($Destination, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($file in $Files) {
                [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.Path, $file.Name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
            }
            foreach ($name in $TextFiles.Keys) {
                $entry = $zip.CreateEntry($name)
                $writer = New-Object System.IO.StreamWriter($entry.Open(), (New-Object System.Text.UTF8Encoding($false)))
                try { $writer.Write($TextFiles[$name]) } finally { $writer.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
}

$releaseZip = Join-Path $packages "Timelapse-v$Version-windows-x64.zip"
$sourceZip = Join-Path $packages "Timelapse-v$Version-source.zip"
$exeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash.ToLowerInvariant()
$releaseFiles = @(
    @{ Path = $executable; Name = 'Timelapse.exe' },
    @{ Path = (Join-Path $projectRoot 'README.md'); Name = 'README.md' }
)
Write-Package $releaseZip $releaseFiles @{ 'SHA256SUMS.txt' = "$exeHash  Timelapse.exe`n" }

# An allowlist keeps local recordings, build outputs, settings and internal
# development notes out of the public repository archive, including hidden files.
$sourceFiles = @()
foreach ($name in @('.gitignore', 'README.md', 'CMakeLists.txt', 'build.ps1', 'package.ps1')) {
    $sourceFiles += @{ Path = (Join-Path $projectRoot $name); Name = $name }
}
foreach ($directory in @('src', 'tests')) {
    foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $projectRoot $directory) -File -Recurse | Sort-Object FullName)) {
        if ($file.Extension -notin @('.cpp', '.h', '.rc', '.manifest')) { continue }
        $relative = $file.FullName.Substring($projectRoot.Length + 1).Replace('\', '/')
        $sourceFiles += @{ Path = $file.FullName; Name = $relative }
    }
}
Write-Package $sourceZip $sourceFiles
$hashes = Get-FileHash -Algorithm SHA256 -LiteralPath $releaseZip, $sourceZip
$hashes | ForEach-Object { $_.Hash.ToLowerInvariant() + '  ' + (Split-Path -Leaf $_.Path) } | Set-Content -LiteralPath (Join-Path $packages 'SHA256SUMS.txt') -Encoding ascii
Get-Item -LiteralPath $releaseZip, $sourceZip | Select-Object Name,Length,FullName
