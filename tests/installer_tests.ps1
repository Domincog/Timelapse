param(
    [Parameter(Mandatory = $true)][string]$Compiler,
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$WorkDirectory
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$Compiler = [System.IO.Path]::GetFullPath($Compiler)
$Executable = [System.IO.Path]::GetFullPath($Executable)
$work = [System.IO.Path]::GetFullPath($WorkDirectory)
if (Test-Path -LiteralPath $work) { throw 'Choose an unused test work directory.' }
[System.IO.Directory]::CreateDirectory($work) | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
$applicationMutex = 'Local\Timelapse.Application.{DC32D155-1B8D-4880-9902-CE6245D34923}'
$maintenanceMutex = 'Local\Timelapse.Setup.{DC32D155-1B8D-4880-9902-CE6245D34923}'
$appId = '{4BCE6D94-85A8-4CE9-BEE5-0998E3C609B0}_is1'
function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Write-Text([string]$Path, [string]$Text) { [System.IO.File]::WriteAllText($Path, $Text, $utf8) }
function Runtime([string]$Path, [string[]]$Arguments) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Path
    $start.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        Assert ($process.WaitForExit(30000)) 'Owned installer test timed out; no app process was terminated.'
        $process.ExitCode
    } finally { $process.Dispose() }
}
function Guard([string]$Name) {
    $created = $false
    $mutex = New-Object System.Threading.Mutex($false, $Name, [ref]$created)
    if (-not $created) { $mutex.Dispose(); throw 'Timelapse or its installer is running. Exit it before testing.' }
    $mutex
}
function Registry-State {
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32, [Microsoft.Win32.RegistryView]::Registry64)) {
        foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryHive]::LocalMachine)) {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, $view)
            try {
                $key = $base.OpenSubKey("Software\Microsoft\Windows\CurrentVersion\Uninstall\$appId")
                if ($key) {
                    try { "$hive/$view exists: " + (($key.GetValueNames() | Sort-Object | ForEach-Object { $_ + '=' + $key.GetValue($_) }) -join ';') }
                    finally { $key.Dispose() }
                } else { "$hive/$view absent" }
            } finally { $base.Dispose() }
        }
    }
}
$beforeRegistry = @(Registry-State)
$payload = Join-Path $work 'payload'
[System.IO.Directory]::CreateDirectory($payload) | Out-Null
Copy-Item -LiteralPath $Executable -Destination (Join-Path $payload 'Timelapse.exe')
Write-Text (Join-Path $payload 'README.md') 'Owned isolated installer payload, revision one.'
Write-Text (Join-Path $payload 'SHA256SUMS.txt') ((Sha $Executable).ToLowerInvariant() + "  Timelapse.exe`n")
$exeHash = Sha $Executable
$sandbox = Join-Path $work 'sandbox'
$app = Join-Path $sandbox 'app'
$common = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/DIR=$app")
$script = Join-Path $project 'installer/Timelapse.iss'

# Compile the actual distribution policy and the identical source with only the
# explicit IsolatedTestRoot destinations/registration overrides. No app launch,
# real Start menu/startup links, or OS Add/Remove Programs entry is performed.
& (Join-Path $project 'installer/build-installer.ps1') -Compiler $Compiler -Version '9.8.7-test' -PayloadDirectory $payload -OutputDirectory (Join-Path $work 'production-policy') | Out-Null
function Build-Isolated([string]$Version) {
    $out = Join-Path $work $Version
    [System.IO.Directory]::CreateDirectory($out) | Out-Null
    & $Compiler '/Qp' "/DAppVersion=$Version" '/DNumericVersion=9.8.7.0' "/DPayloadDirectory=$payload" "/DPackageOutput=$out" "/DIsolatedTestRoot=$sandbox" $script | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'Isolated installer compilation failed.'
    Join-Path $out "Timelapse-v$Version-windows-x64-setup.exe"
}
$setup = Build-Isolated '9.8.7-test'
$guard = Guard $applicationMutex
try {
    Assert ((Runtime $setup ($common + @("/LOG=$(Join-Path $work 'blocked-app.log')"))) -ne 0) 'Running application did not block installation.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $app 'Timelapse.exe'))) 'Blocked setup installed payload.'
} finally { $guard.Dispose() }
$guard = Guard $maintenanceMutex
try {
    Assert ((Runtime $setup ($common + @("/LOG=$(Join-Path $work 'blocked-maintenance.log')"))) -ne 0) 'Concurrent maintenance did not block installation.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $app 'Timelapse.exe'))) 'Concurrent setup installed payload.'
} finally { $guard.Dispose() }
Assert ((Runtime $setup ($common + @("/LOG=$(Join-Path $work 'install-default.log')"))) -eq 0) 'Default isolated installation failed.'
Assert ((Sha (Join-Path $app 'Timelapse.exe')) -eq $exeHash) 'Installed executable differs from frozen payload.'
Assert (Test-Path -LiteralPath (Join-Path $sandbox 'start-menu/Timelapse.lnk')) 'Start menu shortcut missing.'
Assert (-not (Test-Path -LiteralPath (Join-Path $sandbox 'startup/Timelapse.lnk'))) 'Startup was enabled without opting in.'
$shell = New-Object -ComObject WScript.Shell
try {
    $shortcut = $shell.CreateShortcut((Join-Path $sandbox 'start-menu/Timelapse.lnk'))
    Assert ($shortcut.TargetPath -eq (Join-Path $app 'Timelapse.exe')) 'Start menu shortcut targets the wrong executable.'
} finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell) }

# The only user-data sentinels are owned files. Upgrade and uninstall must leave
# unknown files in the app directory and neighboring settings/recordings intact.
Write-Text (Join-Path $app 'settings-sentinel.ini') 'keep app-local user data'
[System.IO.Directory]::CreateDirectory((Join-Path $sandbox 'recordings')) | Out-Null
Write-Text (Join-Path $sandbox 'recordings/owned.mp4') 'keep recordings'
$uninstall = Join-Path $app 'unins000.exe'
$guard = Guard $applicationMutex
try {
    Assert ((Runtime $uninstall @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$(Join-Path $work 'blocked-uninstall.log')")) -ne 0) 'Running application did not block uninstall.'
    Assert ((Sha (Join-Path $app 'Timelapse.exe')) -eq $exeHash) 'Blocked uninstall removed or changed payload.'
    Assert ((Runtime $setup ($common + @("/LOG=$(Join-Path $work 'blocked-upgrade.log')"))) -ne 0) 'Running application did not block upgrade.'
} finally { $guard.Dispose() }
Write-Text (Join-Path $payload 'README.md') 'Owned isolated installer payload, revision two.'
$upgrade = Build-Isolated '9.8.8-test'
Assert ((Runtime $upgrade ($common + @('/TASKS=startup', "/LOG=$(Join-Path $work 'upgrade-opt-in.log')"))) -eq 0) 'Opt-in upgrade failed.'
Assert ([System.IO.File]::ReadAllText((Join-Path $app 'README.md')) -eq 'Owned isolated installer payload, revision two.') 'Upgrade did not replace owned payload.'
$shell = New-Object -ComObject WScript.Shell
try {
    $shortcut = $shell.CreateShortcut((Join-Path $sandbox 'startup/Timelapse.lnk'))
    Assert ($shortcut.TargetPath -eq (Join-Path $app 'Timelapse.exe') -and $shortcut.Arguments -eq '--tray') 'Opt-in startup shortcut does not launch the tray.'
} finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell) }
Assert ((Runtime $upgrade ($common + @('/TASKS=', "/LOG=$(Join-Path $work 'upgrade-opt-out.log')"))) -eq 0) 'Opt-out upgrade failed.'
Assert (-not (Test-Path -LiteralPath (Join-Path $sandbox 'startup/Timelapse.lnk'))) 'Opt-out upgrade retained startup shortcut.'
Assert ((Runtime $uninstall @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$(Join-Path $work 'uninstall.log')")) -eq 0) 'Isolated uninstall failed.'
Assert (-not (Test-Path -LiteralPath (Join-Path $app 'Timelapse.exe'))) 'Uninstall retained executable.'
Assert (-not (Test-Path -LiteralPath (Join-Path $sandbox 'start-menu/Timelapse.lnk'))) 'Uninstall retained Start menu shortcut.'
Assert ([System.IO.File]::ReadAllText((Join-Path $app 'settings-sentinel.ini')) -eq 'keep app-local user data') 'Uninstall changed settings sentinel.'
Assert ([System.IO.File]::ReadAllText((Join-Path $sandbox 'recordings/owned.mp4')) -eq 'keep recordings') 'Uninstall changed recordings sentinel.'
Assert ((@(Registry-State) -join "`n") -eq ($beforeRegistry -join "`n")) 'Isolated test changed OS installation registration.'
[ordered]@{ Passed = $true; Compiler = $Compiler; ExecutableSha256 = $exeHash; SourceSha256 = Sha $script; Policy = 'Same installer source; only registration and destination overrides'; WorkDirectory = $work } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work 'results.json') -Encoding UTF8
Write-Output "PASS: installer lifecycle, running-app/maintenance guards, exact payload, startup opt-in/out, upgrade, uninstall and preservation. Evidence: $work"
