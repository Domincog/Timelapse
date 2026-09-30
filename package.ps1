param([string]$Version = '0.23.0', [string]$BuildDirectory = 'build', [string]$InstallerCompiler = '', [string]$PersonWorker = '')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][A-Za-z0-9.-]+)?$') { throw 'Use a version such as 0.1.0 or 0.1.0-beta.1.' }
$projectRoot = $PSScriptRoot
$executable = Join-Path (Join-Path $projectRoot $BuildDirectory) 'release\Timelapse.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Build and test Release first: .\build.ps1 -Test' }
$packages = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'packages'))
New-Item -ItemType Directory -Path $packages -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Copy-PackageInput([string]$Source, [string]$Destination, [uint64]$ExpectedWorkerBytes = 0) {
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Destination)) | Out-Null
    # Refuse concurrent writes/deletion while copying; later hashes and ZIP reads
    # use only this private copy, even if the build output subsequently changes.
    $inputStream = [System.IO.File]::Open($Source, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        if ($ExpectedWorkerBytes -and $inputStream.Length -ne $ExpectedWorkerBytes) { throw 'Optional person worker size does not match frozen source metadata.' }
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
    foreach ($name in @('.gitignore', 'README.md', 'CMakeLists.txt', 'build.ps1', 'package.ps1', 'tools/verify-encoding-quality.ps1',
        'installer/Timelapse.iss', 'installer/build-installer.ps1', 'tests/installer_tests.ps1',
        'person-pack/README.md', 'person-pack/CMakeLists.txt', 'person-pack/build.ps1',
        'person-pack/model.cpp', 'person-pack/model.h', 'person-pack/model.rc.in', 'person-pack/resources.h', 'person-pack/worker.cpp',
        'person-pack/tests/model_fixture.cpp', 'person-pack/tests/model_tests.cpp', 'person-pack/tests/worker_tests.cpp',
        'person-pack/NOTICE.txt', 'person-pack/NanoDet-LICENSE.txt', 'person-pack/ncnn-LICENSE.txt', 'third-party/ncnn-LICENSE.txt')) {
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
    $personWorkerName = ''
    if ($PersonWorker) {
        # Validate the same immutable metadata that goes into the source ZIP.
        # Development metadata may remain unpinned for ordinary core packaging.
        $metadata = [System.IO.File]::ReadAllText((Join-Path $inputs 'src/person_pack_metadata.h'))
        $names = [regex]::Matches($metadata, '(?m)^\s*inline\s+constexpr\s+wchar_t\s+PersonPackFilename\[\]\s*=\s*L"([^"]+)"\s*;\s*$')
        $sizes = [regex]::Matches($metadata, '(?m)^\s*inline\s+constexpr\s+uint64_t\s+PersonPackExpectedBytes\s*=\s*([0-9]+)(?:ULL|ull)?\s*;\s*$')
        $hashes = [regex]::Matches($metadata, '(?m)^\s*inline\s+constexpr\s+char\s+PersonPackExpectedSha256\[\]\s*=\s*"([0-9a-fA-F]{64})"\s*;\s*$')
        $expectedBytes = [uint64]0
        if ($names.Count -ne 1 -or $names[0].Groups[1].Value -cne 'Timelapse-person-nanodet-r1.exe' -or
            $sizes.Count -ne 1 -or -not [uint64]::TryParse($sizes[0].Groups[1].Value, [ref]$expectedBytes) -or
            $expectedBytes -eq 0 -or $hashes.Count -ne 1) {
            throw 'Optional person worker metadata must contain one pinned filename, positive size and SHA-256 before packaging.'
        }
        $personWorkerName = $names[0].Groups[1].Value
        $frozenWorker = Join-Path $stage $personWorkerName
        Copy-PackageInput ([System.IO.Path]::GetFullPath($PersonWorker)) $frozenWorker $expectedBytes
        if ((Get-Item -LiteralPath $frozenWorker).Length -ne $expectedBytes) { throw 'Optional person worker size does not match frozen source metadata.' }
        $workerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $frozenWorker).Hash.ToLowerInvariant()
        if ($workerHash -cne $hashes[0].Groups[1].Value.ToLowerInvariant()) { throw 'Optional person worker SHA-256 does not match frozen source metadata.' }
    }
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
    $artifactNames = @($releaseName, $sourceName)
    if ($InstallerCompiler) {
        # Compile from the same immutable payload as the portable ZIP. Only the
        # resulting installer participates in publication; compiler tools stay out.
        $payload = Join-Path $stage 'installer-payload'
        Copy-PackageInput $frozenExecutable (Join-Path $payload 'Timelapse.exe')
        Copy-PackageInput (Join-Path $inputs 'README.md') (Join-Path $payload 'README.md')
        [System.IO.File]::WriteAllText((Join-Path $payload 'SHA256SUMS.txt'), "$exeHash  Timelapse.exe`n", (New-Object System.Text.UTF8Encoding($false)))
        & (Join-Path $inputs 'installer/build-installer.ps1') -Compiler $InstallerCompiler -Version $Version -PayloadDirectory $payload -OutputDirectory $stage | Out-Null
        $installerName = "Timelapse-v$Version-windows-x64-setup.exe"
        if (-not (Test-Path -LiteralPath (Join-Path $stage $installerName) -PathType Leaf)) { throw 'Installer compiler produced no setup executable.' }
        $artifactNames += $installerName
    }
    if ($personWorkerName) { $artifactNames += $personWorkerName }
    $hashes = Get-FileHash -Algorithm SHA256 -LiteralPath @($artifactNames | ForEach-Object { Join-Path $stage $_ })
    $hashes | ForEach-Object { $_.Hash.ToLowerInvariant() + '  ' + (Split-Path -Leaf $_.Path) } | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ascii

    $outputs = @()
    foreach ($name in ($artifactNames + @('SHA256SUMS.txt'))) {
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
    Get-Item -LiteralPath @($artifactNames | ForEach-Object { Join-Path $packages $_ }) | Select-Object Name,Length,FullName
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
