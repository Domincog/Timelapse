param([string]$Version = '0.2.0', [string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][A-Za-z0-9.-]+)?$') { throw 'Use a version such as 0.1.0 or 0.1.0-beta.1.' }
$projectRoot = $PSScriptRoot
$executable = Join-Path (Join-Path $projectRoot $BuildDirectory) 'release\Timelapse.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Build and test Release first: .\build.ps1 -Test' }
$packages = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'packages'))
New-Item -ItemType Directory -Path $packages -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Copy-PackageInput([string]$Source, [string]$Destination) {
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Destination)) | Out-Null
    # Refuse concurrent writes/deletion while copying; later hashes and ZIP reads
    # use only this private copy, even if the build output subsequently changes.
    $inputStream = [System.IO.File]::Open($Source, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $outputStream = [System.IO.File]::Open($Destination, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
    } finally { $inputStream.Dispose() }
}

function Write-Package([string]$Destination, [object[]]$Files, [hashtable]$TextFiles = @{}) {
    $stream = [System.IO.File]::Open($Destination, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
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

# Keep the lock file after closing its handle: deleting it could race a second
# invocation that already acquired it. The exclusive handle is the lock.
$packageLock = [System.IO.File]::Open((Join-Path $packages '.package.lock'), [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
$stage = Join-Path $packages ('.package-' + [Guid]::NewGuid().ToString('N'))
$stageCreated = $false
$keepStage = $false
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    $stageCreated = $true
    $inputs = Join-Path $stage 'inputs'

    # An allowlist keeps recordings, build outputs, settings and internal notes
    # out of the source archive. Freeze every selected input before publishing.
    $sourceFiles = @()
    foreach ($name in @('.gitignore', 'README.md', 'CMakeLists.txt', 'build.ps1', 'package.ps1', 'tools/verify-encoding-quality.ps1')) {
        $sourceFiles += @{ Path = (Join-Path $projectRoot $name); Name = $name }
    }
    foreach ($directory in @('src', 'tests')) {
        foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $projectRoot $directory) -File -Recurse | Sort-Object FullName)) {
            $relative = $file.FullName.Substring($projectRoot.Length + 1).Replace('\', '/')
            if ($file.Extension -notin @('.cpp', '.h', '.rc', '.manifest', '.cmake') -and $relative -notin @('tests/package_tests.ps1', 'tests/build_tests.ps1')) { continue }
            $sourceFiles += @{ Path = $file.FullName; Name = $relative }
        }
    }
    foreach ($file in $sourceFiles) {
        $frozen = Join-Path $inputs $file.Name
        Copy-PackageInput $file.Path $frozen
        $file.Path = $frozen
    }
    $frozenExecutable = Join-Path $stage 'Timelapse.exe'
    Copy-PackageInput $executable $frozenExecutable
    $exeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $frozenExecutable).Hash.ToLowerInvariant()
    $releaseName = "Timelapse-v$Version-windows-x64.zip"
    $sourceName = "Timelapse-v$Version-source.zip"
    $releaseZip = Join-Path $stage $releaseName
    $sourceZip = Join-Path $stage $sourceName
    $releaseFiles = @(
        @{ Path = $frozenExecutable; Name = 'Timelapse.exe' },
        @{ Path = (Join-Path $inputs 'README.md'); Name = 'README.md' }
    )
    Write-Package $releaseZip $releaseFiles @{ 'SHA256SUMS.txt' = "$exeHash  Timelapse.exe`n" }
    Write-Package $sourceZip $sourceFiles
    $hashes = Get-FileHash -Algorithm SHA256 -LiteralPath $releaseZip, $sourceZip
    $hashes | ForEach-Object { $_.Hash.ToLowerInvariant() + '  ' + (Split-Path -Leaf $_.Path) } | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ascii

    $outputs = @()
    foreach ($name in @($releaseName, $sourceName, 'SHA256SUMS.txt')) {
        $destination = Join-Path $packages $name
        if (Test-Path -LiteralPath $destination) {
            $item = Get-Item -LiteralPath $destination -Force
            if ($item.PSIsContainer -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                throw "Package destination must be a regular file: $destination"
            }
        }
        $outputs += @{
            Staged = (Join-Path $stage $name)
            Destination = $destination
            Backup = (Join-Path $stage ($name + '.previous'))
            HadOriginal = [System.IO.File]::Exists($destination)
        }
    }
    $attempted = @()
    # Keep recovery files if publication is interrupted before commit/rollback.
    $keepStage = $true
    try {
        foreach ($output in $outputs) {
            # ReplaceFile can move the original to its backup before reporting
            # an error, so include this attempt in rollback before calling it.
            $attempted += $output
            if ($output.HadOriginal) {
                [System.IO.File]::Replace($output.Staged, $output.Destination, $output.Backup)
            } else {
                [System.IO.File]::Move($output.Staged, $output.Destination)
            }
        }
        $keepStage = $false
    } catch {
        $publicationError = $_
        $rollbackErrors = @()
        for ($i = $attempted.Count - 1; $i -ge 0; --$i) {
            $output = $attempted[$i]
            try {
                if ([System.IO.File]::Exists($output.Backup)) {
                    # Retain the backup until every rollback step has succeeded.
                    [System.IO.File]::Copy($output.Backup, $output.Destination, $true)
                } elseif (-not $output.HadOriginal -and -not [System.IO.File]::Exists($output.Staged)) {
                    [System.IO.File]::Delete($output.Destination)
                }
            } catch { $rollbackErrors += $_.Exception.Message }
        }
        if ($rollbackErrors.Count -gt 0) {
            throw "Packaging failed: $($publicationError.Exception.Message) Recovery also failed: $($rollbackErrors -join '; ') Previous files remain in $stage."
        }
        $keepStage = $false
        throw $publicationError
    }
    Get-Item -LiteralPath (Join-Path $packages $releaseName), (Join-Path $packages $sourceName) | Select-Object Name,Length,FullName
} finally {
    try {
        if ($stageCreated -and -not $keepStage) {
            $stagePath = [System.IO.Path]::GetFullPath($stage)
            if ([System.IO.Path]::GetDirectoryName($stagePath) -ne $packages -or ((Get-Item -LiteralPath $stagePath -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                throw "Refusing to remove an unexpected staging path: $stagePath"
            }
            Remove-Item -LiteralPath $stagePath -Recurse -Force
        }
    } catch {
        # Cleanup must not hide the packaging error or undo a successful commit.
        Write-Warning -WarningAction Continue "Could not remove package staging directory ${stage}: $($_.Exception.Message)"
    } finally { $packageLock.Dispose() }
}
