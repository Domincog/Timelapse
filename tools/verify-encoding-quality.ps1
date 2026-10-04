param(
    [Parameter(Mandatory=$true)][string]$InputVideo,
    [Parameter(Mandatory=$true)][ValidateRange(0,3)][int]$Scene,
    [Parameter(Mandatory=$true)][ValidateRange(640,4096)][int]$Width,
    [Parameter(Mandatory=$true)][ValidateRange(360,4096)][int]$Height,
    [Parameter(Mandatory=$true)][ValidateRange(1,100000)][int]$Frames,
    [string]$Ffmpeg='ffmpeg',
    [string]$Ffprobe='ffprobe',
    [string]$Verifier=(Join-Path $PSScriptRoot '../build/Release/encoding_quality_verifier.exe')
)
$ErrorActionPreference='Stop'
if(($Width % 2) -ne 0 -or ($Height % 2) -ne 0){throw 'Corpus dimensions must be even'}
$verifierCommands=@(Get-Command -Name $Verifier -CommandType Application -ErrorAction Stop)
if($verifierCommands.Count -ne 1){throw 'Choose exactly one quality verifier executable'}
$verifierPath=$verifierCommands[0].Source
$video=(Resolve-Path -LiteralPath $InputVideo).Path
$metadataText=& $Ffprobe -v error -select_streams v:0 -show_streams -show_frames -show_entries 'stream=codec_name,width,height,r_frame_rate,time_base,duration,pix_fmt,color_space,color_range,color_transfer,color_primaries:frame=best_effort_timestamp_time,pkt_duration_time' -of json $video
if($LASTEXITCODE -ne 0){throw 'ffprobe could not read input video'}
$metadata=$metadataText | ConvertFrom-Json
if($metadata.streams.Count -ne 1 -or $metadata.streams[0].width -ne $Width -or $metadata.streams[0].height -ne $Height){throw 'Wrong decoded dimensions or stream count'}
if($metadata.streams[0].color_space -ne 'bt709' -or $metadata.streams[0].color_range -ne 'tv' -or $metadata.streams[0].color_transfer -ne 'bt709' -or $metadata.streams[0].color_primaries -ne 'bt709'){throw 'Expected BT.709 limited-range color metadata'}
if($metadata.frames.Count -ne $Frames){throw "Wrong frame count: $($metadata.frames.Count), expected $Frames"}
$duration=[double]::Parse($metadata.streams[0].duration,[Globalization.CultureInfo]::InvariantCulture)
# Permit one validated media-timescale tick for container duration rounding.
# Frame PTS below remains strict; the extra 2us covers ffprobe decimal printing.
$timeBaseParts=$metadata.streams[0].time_base -split '/'
if($timeBaseParts.Count -ne 2){throw 'Invalid stream time base'}
$timeBaseNumerator=[long]::Parse($timeBaseParts[0],[Globalization.CultureInfo]::InvariantCulture)
$timeBaseDenominator=[long]::Parse($timeBaseParts[1],[Globalization.CultureInfo]::InvariantCulture)
if($timeBaseNumerator -lt 1 -or $timeBaseDenominator -lt 1){throw 'Invalid stream time base'}
$mediaTick=[double]$timeBaseNumerator/$timeBaseDenominator
if($mediaTick -gt 0.001){throw 'Stream time base is too coarse for this benchmark'}
if([Math]::Abs($duration-$Frames/30.0) -gt ($mediaTick+0.000002)){throw "Wrong stream duration: $duration"}
for($index=0;$index -lt $Frames;$index++) {
    $stamp=[double]::Parse($metadata.frames[$index].best_effort_timestamp_time,[Globalization.CultureInfo]::InvariantCulture)
    if([Math]::Abs($stamp-$index/30.0) -gt 0.000002){throw "Frame $index timestamp drift or reordering: $stamp"}
}
$metadataText | Set-Content -LiteralPath "$video.ffprobe.json" -Encoding UTF8
$raw=Join-Path (Split-Path -Parent $video) ('decoded-'+[Guid]::NewGuid().ToString('N')+'.nv12')
try {
    & $Ffmpeg -nostdin -hide_banner -loglevel error -threads 1 -i $video -map 0:v:0 -vsync 0 -pix_fmt nv12 -f rawvideo $raw
    if($LASTEXITCODE -ne 0){throw 'ffmpeg decode failed'}
    # Windows PowerShell represents successful native stderr as ErrorRecord;
    # capture its plain text while checking the process exit status explicitly.
    $ErrorActionPreference='Continue'
    # Native commands set the global exit code. A script-local value would
    # shadow it and make a successful verifier appear to have no exit status.
    $global:LASTEXITCODE=$null
    $verificationOutput=& $verifierPath --raw $raw $Scene $Width $Height $Frames 2>&1
    $verificationExit=$global:LASTEXITCODE
    $ErrorActionPreference='Stop'
    $lines=@($verificationOutput | ForEach-Object {$_.ToString()})
    $summaries=@($lines | Where-Object {$_ -like 'SUMMARY *'})
    $frameRows=@($lines | Where-Object {$_ -notlike 'SUMMARY *'})
    if($null -eq $verificationExit -or $verificationExit -ne 0){throw "Quality verification failed: $($lines -join [Environment]::NewLine)"}
    if($summaries.Count -ne 1 -or $summaries[0] -notmatch "^SUMMARY frames=$Frames "){throw 'Quality verifier did not report the expected successful summary'}
    if($frameRows.Count -ne ($Frames+1) -or $frameRows[0] -notlike 'frame,y_psnr,*'){throw 'Quality verifier did not report every frame metric'}
    $frameRows | Set-Content -LiteralPath "$video.quality.csv" -Encoding UTF8
    $summaries | Set-Content -LiteralPath "$video.quality-summary.txt" -Encoding UTF8
    Get-Content -LiteralPath "$video.quality-summary.txt"
} finally {
    # The exact generated file is next to the input benchmark; never
    # enumerate or recursively delete paths supplied by video metadata.
    if(Test-Path -LiteralPath $raw){Remove-Item -LiteralPath $raw}
}
