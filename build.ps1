param([switch]$Test, [string]$SvtAv1Archive = '', [string]$NasmArchive = '')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'The Visual C++ build tools were not found.' }
$vsVersion = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$generator = if ($vsVersion.StartsWith('16.')) { 'Visual Studio 16 2019' } else { 'Visual Studio 17 2022' }
$dependencyArguments = @()
if ($SvtAv1Archive) { $dependencyArguments += "-DTIMELAPSE_SVT_AV1_ARCHIVE=$([System.IO.Path]::GetFullPath($SvtAv1Archive))" }
if ($NasmArchive) { $dependencyArguments += "-DTIMELAPSE_NASM_ARCHIVE=$([System.IO.Path]::GetFullPath($NasmArchive))" }
cmake -S $projectRoot -B (Join-Path $projectRoot 'build') -G $generator -A x64 @dependencyArguments
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build (Join-Path $projectRoot 'build') --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Test) {
    ctest --test-dir (Join-Path $projectRoot 'build') -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
$distribution = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'dist'))
New-Item -ItemType Directory -Path $distribution -Force | Out-Null

function Copy-DistributionInput([string]$Source, [string]$Destination) {
    # Hash and archive only this private copy; reject a concurrent writer while
    # taking the snapshot, even if the original changes after the copy closes.
    $inputStream = [System.IO.File]::Open($Source, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $outputStream = [System.IO.File]::Open($Destination, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
    } finally { $inputStream.Dispose() }
}

# Keep the file after releasing the handle, so a later invocation cannot lose
# its lock when an earlier one finishes. Only distribution publication is locked.
$distributionLock = [System.IO.File]::Open((Join-Path $distribution '.build.lock'), [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
$stage = Join-Path $distribution ('.build-' + [Guid]::NewGuid().ToString('N'))
$stageCreated = $false
$keepStage = $false
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    $stageCreated = $true
    $frozenExecutable = Join-Path $stage 'Timelapse.exe'
    Copy-DistributionInput (Join-Path $projectRoot 'build\release\Timelapse.exe') $frozenExecutable
    $packageFiles = @($frozenExecutable)
    $outputNames = @('Timelapse.exe')
    $readme = Join-Path $projectRoot 'README.md'
    if (Test-Path -LiteralPath $readme) {
        Copy-DistributionInput $readme (Join-Path $stage 'README.md')
        $outputNames += 'README.md'
        $packageFiles += (Join-Path $stage 'README.md')
    } else {
        # Preserve the original optional-README policy: an existing dist copy
        # stays untouched and is included in the ZIP when the project lacks one.
        $readme = Join-Path $distribution 'README.md'
        if (Test-Path -LiteralPath $readme) {
            $item = Get-Item -LiteralPath $readme -Force
            if ($item.PSIsContainer -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                throw "Distribution README must be a regular file: $readme"
            }
            Copy-DistributionInput $readme (Join-Path $stage 'README.md')
            $packageFiles += (Join-Path $stage 'README.md')
        }
    }
    Compress-Archive -LiteralPath $packageFiles -DestinationPath (Join-Path $stage 'Timelapse-portable.zip')
    $hashes = Get-FileHash -Algorithm SHA256 -LiteralPath $frozenExecutable,(Join-Path $stage 'Timelapse-portable.zip')
    $hashes | ForEach-Object { $_.Hash.ToLowerInvariant() + '  ' + (Split-Path -Leaf $_.Path) } | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ascii
    $outputNames += @('Timelapse-portable.zip', 'SHA256SUMS.txt')

    $outputs = @()
    foreach ($name in $outputNames) {
        $destination = Join-Path $distribution $name
        if (Test-Path -LiteralPath $destination) {
            $item = Get-Item -LiteralPath $destination -Force
            if ($item.PSIsContainer -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                throw "Distribution destination must be a regular file: $destination"
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
    $keepStage = $true
    try {
        foreach ($output in $outputs) {
            # ReplaceFile may create its backup before throwing. Journal the
            # attempt first, so that backup is included in handled rollback.
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
                    # Keep recovery bytes until every restoration has succeeded.
                    [System.IO.File]::Copy($output.Backup, $output.Destination, $true)
                } elseif (-not $output.HadOriginal -and -not [System.IO.File]::Exists($output.Staged)) {
                    [System.IO.File]::Delete($output.Destination)
                }
            } catch { $rollbackErrors += $_.Exception.Message }
        }
        if ($rollbackErrors.Count -gt 0) {
            throw "Distribution failed: $($publicationError.Exception.Message) Recovery also failed: $($rollbackErrors -join '; ') Previous files remain in $stage."
        }
        $keepStage = $false
        throw $publicationError
    }
    Get-Item -LiteralPath (Join-Path $distribution 'Timelapse.exe') | Select-Object FullName,Length
} finally {
    try {
        if ($stageCreated -and -not $keepStage) {
            $stagePath = [System.IO.Path]::GetFullPath($stage)
            if ([System.IO.Path]::GetDirectoryName($stagePath) -ne $distribution -or ((Get-Item -LiteralPath $stagePath -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                throw "Refusing to remove an unexpected staging path: $stagePath"
            }
            Remove-Item -LiteralPath $stagePath -Recurse -Force
        }
    } catch {
        Write-Warning -WarningAction Continue "Could not remove distribution staging directory ${stage}: $($_.Exception.Message)"
    } finally { $distributionLock.Dispose() }
}
