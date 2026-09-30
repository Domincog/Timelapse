param(
    [string]$PackageScript = (Join-Path (Split-Path -Parent $PSScriptRoot) 'package.ps1'),
    [string]$WorkDirectory = '',
    [string]$BaselineScript = '',
    [string[]]$CaseName = @()
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$version = '9.8.7-test'
$newVersion = '9.8.8-test'
$retain = -not [string]::IsNullOrEmpty($WorkDirectory)
$scratchParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
$scratchLeaf = 'Timelapse.PackageTests-' + [Guid]::NewGuid().ToString('N')
if (-not $retain) { $WorkDirectory = Join-Path $scratchParent $scratchLeaf }
$workRoot = [System.IO.Path]::GetFullPath($WorkDirectory)
$PackageScript = [System.IO.Path]::GetFullPath($PackageScript)
if ($BaselineScript) { $BaselineScript = [System.IO.Path]::GetFullPath($BaselineScript) }
if (Test-Path -LiteralPath $workRoot) { throw "Refusing to replace existing test work: $workRoot" }
if (-not (Test-Path -LiteralPath $PackageScript -PathType Leaf)) { throw "Package script missing: $PackageScript" }
New-Item -ItemType Directory -Path $workRoot | Out-Null
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$mutationState = @{ Enabled = $false; Done = $false; Path = ''; Text = '' }

function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Write-Text([string]$Path, [string]$Text) { [System.IO.File]::WriteAllText($Path, $Text, $utf8) }
function File-Sha([string]$Path) { (Microsoft.PowerShell.Utility\Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }
function Bytes-Sha([byte[]]$Bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

# A deterministic race seam: return the real hash, then replace only the owned
# original fake EXE. Candidate ZIP/hash reads must use its already frozen copy.
function Get-FileHash {
    [CmdletBinding()]
    param([string]$Algorithm = 'SHA256', [string[]]$LiteralPath)
    $result = Microsoft.PowerShell.Utility\Get-FileHash @PSBoundParameters
    if ($mutationState.Enabled -and -not $mutationState.Done -and $LiteralPath.Count -eq 1 -and
        (Split-Path -Leaf $LiteralPath[0]) -eq 'Timelapse.exe') {
        [System.IO.File]::WriteAllText($mutationState.Path, $mutationState.Text, (New-Object System.Text.UTF8Encoding($false)))
        $mutationState.Done = $true
    }
    $result
}
# Child package scripts have a different script scope. Capture the single state
# object in a closure instead of resolving $script: variables inside the child.
Set-Item -Path Function:Get-FileHash -Value (${function:Get-FileHash}.GetNewClosure())

function New-Fixture([string]$Name, [string]$Script = $PackageScript) {
    $root = Join-Path $workRoot $Name
    New-Item -ItemType Directory -Path $root | Out-Null
    foreach ($dir in @('build/release', 'src', 'tests', 'tools')) { New-Item -ItemType Directory -Path (Join-Path $root $dir) | Out-Null }
    Copy-Item -LiteralPath $Script -Destination (Join-Path $root 'package.ps1')
    Write-Text (Join-Path $root '.gitignore') "build/`npackages/`n"
    Write-Text (Join-Path $root 'README.md') "Owned synthetic package $Name.`n"
    Write-Text (Join-Path $root 'CMakeLists.txt') "# Fixture only; never configured.`n"
    Write-Text (Join-Path $root 'build.ps1') "throw 'Fixture build must never run.'`n"
    Write-Text (Join-Path $root 'src/app.cpp') "// Owned synthetic app input.`n"
    Write-Text (Join-Path $root 'src/helper.h') "// Owned synthetic header input.`n"
    Write-Text (Join-Path $root 'tests/probe.cpp') "// Owned synthetic test input.`n"
    Write-Text (Join-Path $root 'tests/fixture.cmake') "# Owned synthetic CMake test fixture.`n"
    Write-Text (Join-Path $root 'tests/package_tests.ps1') "# Selected packaging regression sentinel.`n"
    Write-Text (Join-Path $root 'tests/build_tests.ps1') "# Selected build distribution regression sentinel.`n"
    Write-Text (Join-Path $root 'tools/verify-encoding-quality.ps1') "# Selected encoding verification sentinel.`n"
    Write-Text (Join-Path $root 'tests/internal.ps1') "# Unselected internal script sentinel.`n"
    Write-Text (Join-Path $root 'src/local.txt') "Unselected local text sentinel.`n"
    Write-Text (Join-Path $root 'private-note.txt') "Unselected root note sentinel.`n"
    Write-Text (Join-Path $root 'build/release/Timelapse.exe') "FAKE EXE ORIGINAL $Name"
    $root
}

function Invoke-Package([string]$Root, [string]$UseVersion = $version) {
    & (Join-Path $Root 'package.ps1') -Version $UseVersion -BuildDirectory 'build' | Out-Null
}

function Get-Inputs([string]$Root, [bool]$ExcludeExe = $false) {
    $map = @{}
    foreach ($file in (Get-ChildItem -LiteralPath $Root -File -Force)) { $map[$file.FullName.Substring($Root.Length + 1)] = File-Sha $file.FullName }
    foreach ($dir in @('src', 'tests', 'build')) {
        foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $Root $dir) -File -Recurse -Force)) {
            if ($ExcludeExe -and $file.Name -eq 'Timelapse.exe') { continue }
            $map[$file.FullName.Substring($Root.Length + 1)] = File-Sha $file.FullName
        }
    }
    $map
}

function Get-Outputs([string]$Root) {
    $map = @{}
    foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $Root 'packages') -File -Force)) {
        if ($file.Name -eq '.package.lock') { continue }
        $map[$file.Name] = File-Sha $file.FullName
    }
    $map
}

function Assert-SameMap([hashtable]$Before, [hashtable]$After, [string]$Message) {
    Assert ($Before.Count -eq $After.Count) ($Message + ' (file count)')
    foreach ($name in $Before.Keys) { Assert ($After.ContainsKey($name) -and $Before[$name] -eq $After[$name]) ($Message + ': ' + $name) }
}

function Assert-Cleanup([string]$Root) {
    $packages = Join-Path $Root 'packages'
    $stages = @(Get-ChildItem -LiteralPath $packages -Force | Where-Object { $_.PSIsContainer -and $_.Name -like '.package-*' })
    Assert ($stages.Count -eq 0) 'Recoverable operation left a staging directory.'
    $lockPath = Join-Path $packages '.package.lock'
    Assert (Test-Path -LiteralPath $lockPath -PathType Leaf) 'Cooperating package lock file is absent.'
    $lock = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    $lock.Dispose()
}

function Read-Release([string]$Root, [string]$UseVersion = $version) {
    $path = Join-Path (Join-Path $Root 'packages') "Timelapse-v$UseVersion-windows-x64.zip"
    $zip = [System.IO.Compression.ZipFile]::OpenRead($path)
    try {
        Assert ($zip.Entries.Count -eq 3) 'Release archive must contain exactly EXE, README, and embedded checksums.'
        $stream = $zip.GetEntry('Timelapse.exe').Open()
        $memory = New-Object System.IO.MemoryStream
        try { $stream.CopyTo($memory); $bytes = $memory.ToArray() } finally { $stream.Dispose(); $memory.Dispose() }
        $reader = New-Object System.IO.StreamReader($zip.GetEntry('SHA256SUMS.txt').Open())
        try { $embedded = $reader.ReadToEnd().Trim() } finally { $reader.Dispose() }
        $readme = $zip.GetEntry('README.md')
        Assert ($null -ne $readme) 'Release archive omitted README.md.'
        $stream = $readme.Open(); $memory = New-Object System.IO.MemoryStream
        try { $stream.CopyTo($memory); $readmeBytes = $memory.ToArray() } finally { $stream.Dispose(); $memory.Dispose() }
        Assert ((Bytes-Sha $readmeBytes) -eq (File-Sha (Join-Path $Root 'README.md'))) 'Release README bytes differ from input.'
        [pscustomobject]@{ ExeText = [System.Text.Encoding]::UTF8.GetString($bytes); ExeHash = Bytes-Sha $bytes; Embedded = $embedded }
    } finally { $zip.Dispose() }
}

function Assert-Package([string]$Root, [string]$UseVersion = $version) {
    $packages = Join-Path $Root 'packages'
    $release = Read-Release $Root $UseVersion
    Assert ($release.Embedded -eq ($release.ExeHash + '  Timelapse.exe')) 'Embedded hash differs from actual archived EXE bytes.'
    $lines = @(Get-Content -LiteralPath (Join-Path $packages 'SHA256SUMS.txt'))
    Assert ($lines.Count -eq 2) 'External checksum index must contain two archives.'
    foreach ($line in $lines) {
        $parts = $line -split '  ', 2
        Assert ($parts.Count -eq 2 -and (File-Sha (Join-Path $packages $parts[1])) -eq $parts[0]) 'External archive checksum mismatch.'
    }
    $expected = @('.gitignore', 'README.md', 'CMakeLists.txt', 'build.ps1', 'package.ps1', 'src/app.cpp', 'src/helper.h', 'tests/probe.cpp', 'tests/fixture.cmake', 'tests/package_tests.ps1', 'tests/build_tests.ps1', 'tools/verify-encoding-quality.ps1')
    $zip = [System.IO.Compression.ZipFile]::OpenRead((Join-Path $packages "Timelapse-v$UseVersion-source.zip"))
    try {
        Assert ($zip.Entries.Count -eq $expected.Count) 'Source allowlist entry count changed.'
        foreach ($name in $expected) {
            $entry = $zip.GetEntry($name)
            Assert ($null -ne $entry) ('Source archive omitted ' + $name)
            $stream = $entry.Open(); $memory = New-Object System.IO.MemoryStream
            try { $stream.CopyTo($memory); $bytes = $memory.ToArray() } finally { $stream.Dispose(); $memory.Dispose() }
            Assert ((Bytes-Sha $bytes) -eq (File-Sha (Join-Path $Root $name))) ('Archived source bytes changed: ' + $name)
        }
    } finally { $zip.Dispose() }
    Assert-Cleanup $Root
}

function Seed-Package([string]$Root) { Invoke-Package $Root; Assert-Package $Root }
function Replace-FakeExe([string]$Root) { Write-Text (Join-Path $Root 'build/release/Timelapse.exe') 'FAKE EXE REPLACEMENT' }

function Assert-StableFailure([string]$Root, [string]$LockedRelative = '', [string]$UseVersion = $version, [bool]$Exclusive = $false) {
    $inputs = Get-Inputs $Root
    $outputs = Get-Outputs $Root
    $held = $null
    if ($LockedRelative) {
        $access = [System.IO.FileAccess]::Read
        $sharing = [System.IO.FileShare]::Read
        if ($Exclusive) { $access = [System.IO.FileAccess]::ReadWrite; $sharing = [System.IO.FileShare]::None }
        $held = [System.IO.File]::Open((Join-Path $Root $LockedRelative), [System.IO.FileMode]::Open, $access, $sharing)
    }
    $failure = $null
    try { Invoke-Package $Root $UseVersion } catch { $failure = $_.Exception.Message }
    finally { if ($held) { $held.Dispose() } }
    Assert ($null -ne $failure) 'Expected packaging operation to fail.'
    Assert-SameMap $inputs (Get-Inputs $Root) 'Packaging altered source/input files'
    Assert-SameMap $outputs (Get-Outputs $Root) 'Failed packaging altered final output set'
    Assert-Cleanup $Root
    $failure
}

$cases = @(
    @{ Name = 'success_and_allowlist'; Body = {
        param($root)
        $before = Get-Inputs $root; Seed-Package $root
        Assert-SameMap $before (Get-Inputs $root) 'Successful packaging altered inputs'
        Replace-FakeExe $root
        $before = Get-Inputs $root; Invoke-Package $root; Assert-Package $root
        Assert-SameMap $before (Get-Inputs $root) 'Successful replacement altered inputs'
        Assert ((Read-Release $root).ExeText -eq 'FAKE EXE REPLACEMENT') 'Successful replacement did not update executable.'
    } },
    @{ Name = 'missing_required_input'; Body = {
        param($root)
        Seed-Package $root; Replace-FakeExe $root
        Remove-Item -LiteralPath (Join-Path $root 'CMakeLists.txt')
        Assert-StableFailure $root
    } },
    @{ Name = 'locked_source_input'; Body = {
        param($root)
        Seed-Package $root; Replace-FakeExe $root
        Assert-StableFailure $root 'src/app.cpp' $version $true
    } },
    @{ Name = 'locked_source_archive'; Body = {
        param($root)
        Seed-Package $root; Replace-FakeExe $root
        Assert-StableFailure $root "packages/Timelapse-v$version-source.zip"
        Assert-Package $root
    } },
    @{ Name = 'locked_checksum_index'; Body = {
        param($root)
        Seed-Package $root; Replace-FakeExe $root
        Assert-StableFailure $root 'packages/SHA256SUMS.txt'
        Assert-Package $root
    } },
    @{ Name = 'new_version_locked_index'; Body = {
        param($root)
        Seed-Package $root; Replace-FakeExe $root
        Assert-StableFailure $root 'packages/SHA256SUMS.txt' $newVersion
        Assert-Package $root
        Assert (-not (Test-Path -LiteralPath (Join-Path $root "packages/Timelapse-v$newVersion-windows-x64.zip"))) 'New release survived failed publication.'
        Assert (-not (Test-Path -LiteralPath (Join-Path $root "packages/Timelapse-v$newVersion-source.zip"))) 'New source survived failed publication.'
    } },
    @{ Name = 'input_mutation_after_hash'; Body = {
        param($root)
        $before = Get-Inputs $root $true
        $original = [System.IO.File]::ReadAllText((Join-Path $root 'build/release/Timelapse.exe'))
        $mutationState.Path = Join-Path $root 'build/release/Timelapse.exe'
        $mutationState.Text = 'FAKE EXE CHANGED AFTER HASH'
        $mutationState.Enabled = $true; $mutationState.Done = $false
        try { Invoke-Package $root } finally { $mutationState.Enabled = $false }
        Assert $mutationState.Done 'Deterministic executable mutation did not run.'
        Assert ((Read-Release $root).ExeText -eq $original) 'Archive read mutable original after computing checksum.'
        Assert-Package $root
        Assert-SameMap $before (Get-Inputs $root $true) 'Mutation test altered source/test inputs'
        Assert ([System.IO.File]::ReadAllText($mutationState.Path) -eq $mutationState.Text) 'Original executable mutation was lost.'
    } },
    @{ Name = 'cooperative_publication_lock'; Body = {
        param($root)
        Seed-Package $root; Replace-FakeExe $root
        Assert-StableFailure $root 'packages/.package.lock' $version $true
        Assert-Package $root
    } }
)
$results = @()
if ($CaseName.Count) {
    foreach ($name in $CaseName) { Assert ($name -in $cases.Name) ('Unknown selected test case: ' + $name) }
    $cases = @($cases | Where-Object { $_.Name -in $CaseName })
}
foreach ($case in $cases) {
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $failure = $null; $details = @()
    try { $root = New-Fixture $case.Name; $details = @(& $case.Body $root) }
    catch { $failure = $_.Exception.Message }
    finally { $timer.Stop(); $mutationState.Enabled = $false }
    $passed = $null -eq $failure
    $results += [pscustomobject]@{ Name = $case.Name; Passed = $passed; ElapsedMs = $timer.ElapsedMilliseconds; Failure = $failure; Details = $details }
    Write-Output ("{0}: passed={1} elapsed_ms={2}{3}" -f $case.Name, $passed, $timer.ElapsedMilliseconds, $(if ($failure) { ' error=' + $failure } else { '' }))
}

if ($BaselineScript) {
    $failure = $null
    try {
        $root = New-Fixture 'baseline_hash_race_control' $BaselineScript
        $mutationState.Path = Join-Path $root 'build/release/Timelapse.exe'
        $mutationState.Text = 'FAKE EXE CHANGED AFTER HASH'
        $mutationState.Enabled = $true; $mutationState.Done = $false
        try { Invoke-Package $root } finally { $mutationState.Enabled = $false }
        $release = Read-Release $root
        Assert $mutationState.Done 'Baseline mutation did not execute.'
        Assert ($release.ExeText -eq $mutationState.Text) 'Baseline archive did not pick up changed executable.'
        Assert ($release.Embedded -ne ($release.ExeHash + '  Timelapse.exe')) 'Baseline unexpectedly resisted hash/copy race.'
    } catch { $failure = $_.Exception.Message }
    $results += [pscustomobject]@{ Name = 'baseline_hash_race_control'; Passed = $null -eq $failure; Failure = $failure }
    Write-Output ('baseline_hash_race_control: reproduced=' + ($null -eq $failure))
}
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $workRoot 'results.json') -Encoding UTF8
[ordered]@{ PowerShellVersion = $PSVersionTable.PSVersion.ToString(); PackageScript = $PackageScript; PackageScriptSha256 = File-Sha $PackageScript; FixtureSha256 = File-Sha $PSCommandPath } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $workRoot 'inputs.json') -Encoding UTF8
$failed = @($results | Where-Object { -not $_.Passed }).Count
Write-Output ("results={0}/{1} work={2}" -f ($results.Count - $failed), $results.Count, $workRoot)
if (-not $retain -and -not $failed) {
    # Only remove the unique root created by this invocation, never a supplied
    # directory or a computed path outside the intended temporary parent.
    $resolved = [System.IO.Path]::GetFullPath($workRoot)
    $item = Get-Item -LiteralPath $resolved -Force
    Assert ([System.IO.Path]::GetDirectoryName($resolved) -eq $scratchParent -and
        [System.IO.Path]::GetFileName($resolved) -eq $scratchLeaf -and
        -not ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) 'Unsafe owned scratch cleanup path.'
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
if ($failed) { exit 1 }
exit 0
