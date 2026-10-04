param(
    [string]$BuildScript = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build.ps1'),
    [string]$WorkDirectory = '',
    [string[]]$CaseName = @()
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$retain = -not [string]::IsNullOrEmpty($WorkDirectory)
$scratchParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
$scratchLeaf = 'Timelapse.BuildTests-' + [Guid]::NewGuid().ToString('N')
if (-not $retain) { $WorkDirectory = Join-Path $scratchParent $scratchLeaf }
$workRoot = [System.IO.Path]::GetFullPath($WorkDirectory)
$BuildScript = [System.IO.Path]::GetFullPath($BuildScript)
if (Test-Path -LiteralPath $workRoot) { throw "Refusing to replace existing test work: $workRoot" }
if (-not (Test-Path -LiteralPath $BuildScript -PathType Leaf)) { throw "Build script missing: $BuildScript" }
# The actual script queries installed vswhere read-only. Every configure/build/
# test command is intercepted below; the fake EXE is text and is never executed.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'These build-script tests require the installed Visual Studio Build Tools discovery utility.' }
New-Item -ItemType Directory -Path $workRoot | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Load the real archive module before defining the seam; auto-import must not
# overwrite it on the first qualified call.
Import-Module Microsoft.PowerShell.Archive
$commandState = @{
    ConfigureExit = 0; BuildExit = 0; TestExit = 0
    Calls = (New-Object 'System.Collections.Generic.List[object]')
    FailArchive = $false; MutatePath = ''; MutationDone = $false
}

function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Write-Text([string]$Path, [string]$Text) { [System.IO.File]::WriteAllText($Path, $Text, $utf8) }
function File-Sha([string]$Path) { (Microsoft.PowerShell.Utility\Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }
function Bytes-Sha([byte[]]$Bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function cmake {
    $kind = if ($args[0] -eq '--build') { 'build' } else { 'configure' }
    $commandState.Calls.Add([pscustomobject]@{ Command = $kind; Arguments = @($args) })
    $global:LASTEXITCODE = if ($kind -eq 'build') { $commandState.BuildExit } else { $commandState.ConfigureExit }
}
function ctest {
    $commandState.Calls.Add([pscustomobject]@{ Command = 'test'; Arguments = @($args) })
    $global:LASTEXITCODE = $commandState.TestExit
}
function Compress-Archive {
    [CmdletBinding()]
    param([string[]]$LiteralPath, [string]$DestinationPath, [switch]$Force)
    if ($commandState.FailArchive) { throw 'Owned synthetic archive failure.' }
    # Mutate only the fake original after the candidate has copied its input,
    # immediately before the real archive reads its supplied paths.
    if ($commandState.MutatePath -and -not $commandState.MutationDone) {
        [System.IO.File]::WriteAllText($commandState.MutatePath, 'DUMMY MUTATED INPUT - NOT RUNNABLE', (New-Object System.Text.UTF8Encoding($false)))
        $commandState.MutationDone = $true
    }
    Microsoft.PowerShell.Archive\Compress-Archive @PSBoundParameters
}
# Child scripts have their own script scope; each seam closes over the same
# explicit state object rather than resolving a child $script: variable.
foreach ($name in @('cmake', 'ctest', 'Compress-Archive')) {
    $body = (Get-Item -LiteralPath ('Function:' + $name)).ScriptBlock.GetNewClosure()
    Set-Item -Path ('Function:' + $name) -Value $body
}

function New-Fixture([string]$Name) {
    $root = Join-Path $workRoot $Name
    New-Item -ItemType Directory -Path (Join-Path $root 'build/release') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $root 'dist/updated') -Force | Out-Null
    Copy-Item -LiteralPath $BuildScript -Destination (Join-Path $root 'build.ps1')
    Write-Text (Join-Path $root 'build/release/Timelapse.exe') 'DUMMY ORIGINAL INPUT - NOT RUNNABLE'
    Write-Text (Join-Path $root 'README.md') 'Owned original README'
    Write-Text (Join-Path $root 'dist/unrelated.txt') 'Unrelated distribution file must survive.'
    Write-Text (Join-Path $root 'dist/updated/marker.txt') 'Unrelated nested staged milestone must survive.'
    $root
}
function Invoke-Build([string]$Root, [bool]$RunTests = $true, [string]$SvtArchive = '', [string]$NasmArchive = '') {
    & (Join-Path $Root 'build.ps1') -Test:$RunTests -SvtAv1Archive $SvtArchive -NasmArchive $NasmArchive | Out-Null
}
function Get-Inputs([string]$Root, [bool]$ExcludeExe = $false) {
    $map = @{}
    foreach ($file in (Get-ChildItem -LiteralPath $Root -File -Force)) { $map[$file.Name] = File-Sha $file.FullName }
    foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $Root 'build') -File -Recurse -Force)) {
        if ($ExcludeExe -and $file.Name -eq 'Timelapse.exe') { continue }
        $map[$file.FullName.Substring($Root.Length + 1)] = File-Sha $file.FullName
    }
    $map
}
function Get-Outputs([string]$Root) {
    $dist = Join-Path $Root 'dist'
    $map = @{}
    foreach ($file in (Get-ChildItem -LiteralPath $dist -File -Recurse -Force)) {
        if ($file.FullName -eq (Join-Path $dist '.build.lock')) { continue }
        $map[$file.FullName.Substring($dist.Length + 1)] = File-Sha $file.FullName
    }
    $map
}
function Assert-SameMap([hashtable]$Before, [hashtable]$After, [string]$Message) {
    Assert ($Before.Count -eq $After.Count) ($Message + ' (file count)')
    foreach ($name in $Before.Keys) { Assert ($After.ContainsKey($name) -and $Before[$name] -eq $After[$name]) ($Message + ': ' + $name) }
}
function Assert-Cleanup([string]$Root) {
    $dist = Join-Path $Root 'dist'
    $stages = @(Get-ChildItem -LiteralPath $dist -Force | Where-Object { $_.PSIsContainer -and $_.Name -like '.build-*' })
    Assert ($stages.Count -eq 0) 'Recoverable operation left a staging directory.'
    $lockPath = Join-Path $dist '.build.lock'
    Assert (Test-Path -LiteralPath $lockPath -PathType Leaf) 'Distribution lock file is absent.'
    $held = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    $held.Dispose()
}
function Read-EntryHash($Entry) {
    $stream = $Entry.Open(); $memory = New-Object System.IO.MemoryStream
    try { $stream.CopyTo($memory); Bytes-Sha $memory.ToArray() }
    finally { $stream.Dispose(); $memory.Dispose() }
}
function Assert-Distribution([string]$Root, [bool]$HasReadme = $true) {
    $dist = Join-Path $Root 'dist'
    $zip = [System.IO.Compression.ZipFile]::OpenRead((Join-Path $dist 'Timelapse-portable.zip'))
    try {
        $expectedCount = if ($HasReadme) { 2 } else { 1 }
        Assert ($zip.Entries.Count -eq $expectedCount) 'Portable archive entry count changed.'
        foreach ($name in @('Timelapse.exe') + $(if ($HasReadme) { @('README.md') } else { @() })) {
            $entry = $zip.GetEntry($name)
            Assert ($null -ne $entry) ('Portable archive omitted ' + $name)
            Assert ((Read-EntryHash $entry) -eq (File-Sha (Join-Path $dist $name))) ('Archived bytes differ from published ' + $name)
        }
    } finally { $zip.Dispose() }
    $lines = @(Get-Content -LiteralPath (Join-Path $dist 'SHA256SUMS.txt'))
    Assert ($lines.Count -eq 2) 'Checksum index must have two entries.'
    $expectedNames = @('Timelapse.exe', 'Timelapse-portable.zip')
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        $parts = $lines[$i] -split '  ', 2
        Assert ($parts.Count -eq 2 -and $parts[1] -eq $expectedNames[$i]) 'Checksum index names/order changed.'
        Assert ($parts[0] -eq (File-Sha (Join-Path $dist $parts[1]))) 'Checksum differs from final file bytes.'
    }
    Assert ([System.IO.File]::ReadAllText((Join-Path $dist 'unrelated.txt')) -eq 'Unrelated distribution file must survive.') 'Unrelated file changed.'
    Assert ([System.IO.File]::ReadAllText((Join-Path $dist 'updated/marker.txt')) -eq 'Unrelated nested staged milestone must survive.') 'Unrelated nested file changed.'
    Assert-Cleanup $Root
}
function Seed-Distribution([string]$Root) { Invoke-Build $Root; Assert-Distribution $Root }
function Replace-Inputs([string]$Root) {
    Write-Text (Join-Path $Root 'build/release/Timelapse.exe') 'DUMMY REPLACEMENT INPUT - NOT RUNNABLE'
    Write-Text (Join-Path $Root 'README.md') 'Owned replacement README'
}
function Assert-StableFailure([string]$Root, [string]$LockedRelative = '', [bool]$Exclusive = $false, [string]$Expected = '') {
    $inputs = Get-Inputs $Root; $outputs = Get-Outputs $Root
    $held = $null
    if ($LockedRelative) {
        $access = [System.IO.FileAccess]::Read; $sharing = [System.IO.FileShare]::Read
        if ($Exclusive) { $access = [System.IO.FileAccess]::ReadWrite; $sharing = [System.IO.FileShare]::None }
        $held = [System.IO.File]::Open((Join-Path $Root $LockedRelative), [System.IO.FileMode]::Open, $access, $sharing)
    }
    $failure = $null
    try { Invoke-Build $Root } catch { $failure = $_.Exception.Message }
    finally { if ($held) { $held.Dispose() } }
    Assert ($null -ne $failure) 'Expected build/distribution operation to fail.'
    if ($Expected) { Assert ($failure.Contains($Expected)) ('Wrong failure stage: ' + $failure) }
    Assert-SameMap $inputs (Get-Inputs $Root) 'Failure altered project/build inputs'
    Assert-SameMap $outputs (Get-Outputs $Root) 'Failure altered final distribution bytes/set'
    Assert-Cleanup $Root
    $failure
}

$cases = @(
    @{ Name = 'success_and_replacement'; Body = {
        param($root)
        $before = Get-Inputs $root; $callStart = $commandState.Calls.Count
        Invoke-Build $root $false; Assert-Distribution $root
        Assert-SameMap $before (Get-Inputs $root) 'Successful build publisher altered inputs'
        Assert ($commandState.Calls.Count - $callStart -eq 2) 'Build without -Test did not call exactly configure/build.'
        Assert ($commandState.Calls[$callStart].Command -eq 'configure' -and $commandState.Calls[$callStart + 1].Command -eq 'build') 'Wrong build command order.'
        Replace-Inputs $root
        $before = Get-Inputs $root; $callStart = $commandState.Calls.Count
        Invoke-Build $root; Assert-Distribution $root
        Assert-SameMap $before (Get-Inputs $root) 'Replacement altered inputs'
        Assert ($commandState.Calls.Count - $callStart -eq 3 -and $commandState.Calls[$callStart + 2].Command -eq 'test') 'Build -Test did not invoke CTest after build.'
        Assert ((File-Sha (Join-Path $root 'build/release/Timelapse.exe')) -eq (File-Sha (Join-Path $root 'dist/Timelapse.exe'))) 'New executable was not published.'
        Assert ((File-Sha (Join-Path $root 'README.md')) -eq (File-Sha (Join-Path $root 'dist/README.md'))) 'Current README was not published.'
        $svtArchive = '.tmp/offline archives/SVT-AV1 ü.tar.gz'
        $nasmArchive = '.tmp/offline archives/NASM ü.zip'
        $callStart = $commandState.Calls.Count
        Invoke-Build $root $false $svtArchive $nasmArchive; Assert-Distribution $root
        $arguments = $commandState.Calls[$callStart].Arguments
        Assert ($arguments.Count -eq 10 -and
            "-DTIMELAPSE_SVT_AV1_ARCHIVE=$([IO.Path]::GetFullPath($svtArchive))" -cin $arguments -and
            "-DTIMELAPSE_NASM_ARCHIVE=$([IO.Path]::GetFullPath($nasmArchive))" -cin $arguments) 'Offline archive paths were omitted, split, or not resolved.'
    } },
    @{ Name = 'early_command_failures'; Body = {
        param($root)
        Seed-Distribution $root; Replace-Inputs $root
        foreach ($failureStage in @('configure', 'build', 'test')) {
            $callStart = $commandState.Calls.Count
            $property = @{ configure = 'ConfigureExit'; build = 'BuildExit'; test = 'TestExit' }[$failureStage]
            $expected = @{ configure = 'CMake configuration failed.'; build = 'Build failed.'; test = 'Tests failed.' }[$failureStage]
            $count = @{ configure = 1; build = 2; test = 3 }[$failureStage]
            $commandState[$property] = 19
            try { Assert-StableFailure $root '' $false $expected } finally { $commandState[$property] = 0 }
            Assert ($commandState.Calls.Count - $callStart -eq $count) 'Script continued past failed configure/build/test command.'
        }
        Assert-Distribution $root
    } },
    @{ Name = 'locked_existing_outputs'; Body = {
        param($root)
        Seed-Distribution $root; Replace-Inputs $root
        foreach ($name in @('README.md', 'Timelapse-portable.zip', 'SHA256SUMS.txt')) {
            Assert-StableFailure $root ('dist/' + $name)
            Assert-Distribution $root
        }
        Invoke-Build $root; Assert-Distribution $root
    } },
    @{ Name = 'new_outputs_locked_index'; Body = {
        param($root)
        Write-Text (Join-Path $root 'dist/SHA256SUMS.txt') 'Owned preexisting index without managed artifacts'
        Assert-StableFailure $root 'dist/SHA256SUMS.txt'
        foreach ($name in @('Timelapse.exe', 'README.md', 'Timelapse-portable.zip')) {
            Assert (-not (Test-Path -LiteralPath (Join-Path $root ('dist/' + $name)))) ('New output survived failed publication: ' + $name)
        }
        Invoke-Build $root; Assert-Distribution $root
    } },
    @{ Name = 'optional_readme_fallback'; Body = {
        param($root)
        Seed-Distribution $root; Replace-Inputs $root
        Remove-Item -LiteralPath (Join-Path $root 'README.md')
        $readmePath = Join-Path $root 'dist/README.md'; $before = File-Sha $readmePath
        $held = [System.IO.File]::Open($readmePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        try { Invoke-Build $root; Assert-Distribution $root } finally { $held.Dispose() }
        Assert ((File-Sha $readmePath) -eq $before) 'Missing project README rewrote the retained dist README.'
        Assert ((File-Sha (Join-Path $root 'build/release/Timelapse.exe')) -eq (File-Sha (Join-Path $root 'dist/Timelapse.exe'))) 'Fallback README prevented executable update.'
    } },
    @{ Name = 'optional_readme_absent'; Body = {
        param($root)
        Remove-Item -LiteralPath (Join-Path $root 'README.md')
        Invoke-Build $root; Assert-Distribution $root $false
        Assert (-not (Test-Path -LiteralPath (Join-Path $root 'dist/README.md'))) 'Absent optional README was created.'
    } },
    @{ Name = 'input_and_archive_failures'; Body = {
        param($root)
        Seed-Distribution $root; Replace-Inputs $root
        Assert-StableFailure $root 'build/release/Timelapse.exe' $true
        $commandState.FailArchive = $true
        try { Assert-StableFailure $root '' $false 'Owned synthetic archive failure.' } finally { $commandState.FailArchive = $false }
        Remove-Item -LiteralPath (Join-Path $root 'build/release/Timelapse.exe')
        Assert-StableFailure $root
        Assert-Distribution $root
    } },
    @{ Name = 'input_mutation_before_archive'; Body = {
        param($root)
        $before = Get-Inputs $root $true
        $path = Join-Path $root 'build/release/Timelapse.exe'; $original = File-Sha $path
        $commandState.MutatePath = $path; $commandState.MutationDone = $false
        try { Invoke-Build $root } finally { $commandState.MutatePath = '' }
        Assert $commandState.MutationDone 'Owned executable mutation did not execute.'
        Assert ((File-Sha $path) -ne $original) 'Mutation did not change original input.'
        Assert ((File-Sha (Join-Path $root 'dist/Timelapse.exe')) -eq $original) 'Publisher reread changing original after input snapshot.'
        Assert-Distribution $root
        Assert-SameMap $before (Get-Inputs $root $true) 'Mutation case altered other inputs'
    } },
    @{ Name = 'invalid_destination'; Body = {
        param($root)
        New-Item -ItemType Directory -Path (Join-Path $root 'dist/Timelapse-portable.zip') | Out-Null
        Assert-StableFailure $root '' $false 'must be a regular file'
        Assert (Test-Path -LiteralPath (Join-Path $root 'dist/Timelapse-portable.zip') -PathType Container) 'Invalid destination directory was changed.'
        Assert (-not (Test-Path -LiteralPath (Join-Path $root 'dist/Timelapse.exe'))) 'Publisher changed files before destination validation.'
    } },
    @{ Name = 'cooperative_publication_lock'; Body = {
        param($root)
        Seed-Distribution $root; Replace-Inputs $root
        Assert-StableFailure $root 'dist/.build.lock' $true
        Assert-Distribution $root
    } }
)
if ($CaseName.Count) {
    foreach ($name in $CaseName) { Assert ($name -in $cases.Name) ('Unknown selected test case: ' + $name) }
    $cases = @($cases | Where-Object { $_.Name -in $CaseName })
}
$results = @()
foreach ($case in $cases) {
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $failure = $null; $details = @()
    try { $root = New-Fixture $case.Name; $details = @(& $case.Body $root) }
    catch { $failure = $_.Exception.Message }
    finally {
        $timer.Stop()
        $commandState.ConfigureExit = 0; $commandState.BuildExit = 0; $commandState.TestExit = 0
        $commandState.FailArchive = $false; $commandState.MutatePath = ''
    }
    $passed = $null -eq $failure
    $results += [pscustomobject]@{ Name = $case.Name; Passed = $passed; ElapsedMs = $timer.ElapsedMilliseconds; Failure = $failure; Details = $details }
    Write-Output ("{0}: passed={1} elapsed_ms={2}{3}" -f $case.Name, $passed, $timer.ElapsedMilliseconds, $(if ($failure) { ' error=' + $failure } else { '' }))
}
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $workRoot 'results.json') -Encoding UTF8
[ordered]@{ PowerShellVersion = $PSVersionTable.PSVersion.ToString(); BuildScript = $BuildScript; BuildScriptSha256 = File-Sha $BuildScript; FixtureSha256 = File-Sha $PSCommandPath; StubCalls = @($commandState.Calls.ToArray()) } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $workRoot 'inputs.json') -Encoding UTF8
$failed = @($results | Where-Object { -not $_.Passed }).Count
Write-Output ("results={0}/{1} work={2}" -f ($results.Count - $failed), $results.Count, $workRoot)
if (-not $retain -and -not $failed) {
    # Only delete the unique default scratch root created by this invocation.
    # Explicit work paths and all failure evidence are retained for inspection.
    $resolved = [System.IO.Path]::GetFullPath($workRoot)
    $item = Get-Item -LiteralPath $resolved -Force
    Assert ([System.IO.Path]::GetDirectoryName($resolved) -eq $scratchParent -and
        [System.IO.Path]::GetFileName($resolved) -eq $scratchLeaf -and
        -not ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) 'Unsafe owned scratch cleanup path.'
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
if ($failed) { exit 1 }
exit 0
