param(
    [Parameter(Mandatory=$true)][string]$NcnnArchive,
    [Parameter(Mandatory=$true)][string]$ModelArchive,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
# Explicit local archives keep a normal recorder build network-free. Each run
# takes a private immutable snapshot before verifying and expanding dependencies.
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$run = Join-Path $output ('build-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
function Freeze-Archive([string]$Source, [string]$Name, [string]$Hash) {
    $destination = Join-Path $run $Name
    $inputStream = [IO.File]::Open([IO.Path]::GetFullPath($Source), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $outputStream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
    } finally { $inputStream.Dispose() }
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $Hash) { throw "Pinned archive hash mismatch: $Name" }
    return $destination
}
$runtime = Freeze-Archive $NcnnArchive 'ncnn.zip' '754659D6FE65545CF2EF4483FFB84526FEA631F8764C44B150F1601D0FB4004B'
$model = Freeze-Archive $ModelArchive 'nanodet.zip' '2181FF5091E70B5EB39B5B245F1694C92BCA536523C6EE11BE81CCDA61691392'
Expand-Archive -LiteralPath $runtime -DestinationPath (Join-Path $run 'runtime')
Expand-Archive -LiteralPath $model -DestinationPath (Join-Path $run 'model')
$runtimeSource = Join-Path $run 'runtime'
$paramFiles = @(Get-ChildItem -LiteralPath (Join-Path $run 'model') -Recurse -File -Filter 'nanodet_m.param')
if (-not (Test-Path -LiteralPath (Join-Path $runtimeSource 'src/net.cpp')) -or $paramFiles.Count -ne 1) { throw 'Unexpected pinned dependency archive layout.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$version = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if (-not $version) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$generator = if ($version.StartsWith('16.')) { 'Visual Studio 16 2019' } else { 'Visual Studio 17 2022' }
$build = Join-Path $run 'compiled'
cmake -S $PSScriptRoot -B $build -G $generator -A x64 "-DNCNN_SOURCE_DIR=$runtimeSource" "-DPERSON_MODEL_DIR=$($paramFiles[0].DirectoryName)" -DPERSON_PACK_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw 'Person worker configuration failed.' }
cmake --build $build --config Release --parallel 3
if ($LASTEXITCODE -ne 0) { throw 'Person worker build failed.' }
ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Person worker tests failed.' }
$executable = Join-Path $build 'Release/TimelapsePersonWorker.exe'
$manifest = [ordered]@{
    ProtocolRevision=1; ModelRevision=1; PreprocessingRevision=1
    Executable=$executable; Bytes=(Get-Item -LiteralPath $executable).Length
    SHA256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    NcnnCommit='e54f7b1f88434e1d844ea0551b880a1cfb079ce1'
    NanoDetCommit='c63d7cb0b9bfb2e742d9cc8d3fb26eb00df3221e'
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'worker-manifest.json') -Encoding UTF8
$manifest | ConvertTo-Json
