# Psychonauts VR mod - installer / uninstaller.
# Windows PowerShell 5.1. Run through Install.bat / Uninstall.bat.
#
#   -Game <path>       the game folder (the one with Psychonauts.exe); found automatically if omitted
#   -Uninstall         remove the mod instead
#   -NoPrompt          never ask, never wait for Enter (tests, scripts)
#   -Force             install even though Psychonauts.exe is not the supported Steam build
#   -SteamRoot <path>  use this Steam folder instead of the registry (tests)
#   -Source <dir>      the folder with the files to install (default: ..\Game files)
#
# Psychonauts.exe, psyvr.ini, psyvr.log and saves (profiles\) are never
# modified or deleted. DisplaySettings.ini is changed by the mod itself at its
# first VR start (it keeps the player's copy as DisplaySettings.ini.psyvr-backup);
# Uninstall puts that copy back.
param(
    [string]$Game = '',
    [switch]$Uninstall,
    [switch]$NoPrompt,
    [switch]$Force,
    [string]$SteamRoot = '',
    [string]$Source = ''
)

$ErrorActionPreference = 'Stop'
$AppId = '3830'
$SupportedStamp = [uint32]0x568C5772
$ManifestName = 'psyvr-install.txt'
$Protected = @('psychonauts.exe', 'psyvr.ini', 'psyvr.log')
$KnownFiles = @('d3d9.dll', 'psyvr_logic.dll', 'd3d9_dxvk.dll',
    'Play Psychonauts VR.bat', 'Play Psychonauts UNMODDED.bat')
# The optional Astralathe part: copied only when Astralathe is installed
# (ModResource exists). The mod writes its Scripts\Dart_hooks.lua at start-up.
$AstraDir = 'ModResource\PsyVR'

function Wait-Enter {
    if (-not $NoPrompt) {
        Write-Host ''
        [void](Read-Host 'Press Enter to close this window')
    }
}

function Stop-With([string]$Message) {
    Write-Host ''
    Write-Host ('ERROR: ' + $Message) -ForegroundColor Red
    Wait-Enter
    exit 1
}

function Test-GameFolder([string]$Dir) {
    if (-not $Dir) { return $false }
    return (Test-Path -LiteralPath (Join-Path $Dir 'Psychonauts.exe') -PathType Leaf)
}

function Get-SteamRootPath {
    if ($SteamRoot) { return $SteamRoot }
    $candidates = @(
        @('HKCU:\Software\Valve\Steam', 'SteamPath'),
        @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath')
    )
    foreach ($c in $candidates) {
        try {
            $v = (Get-ItemProperty -Path $c[0] -Name $c[1] -ErrorAction Stop).($c[1])
            if ($v) {
                $v = ([string]$v).Replace('/', '\')
                if (Test-Path -LiteralPath $v -PathType Container) { return $v }
            }
        } catch { }
    }
    return ''
}

function Find-GameViaSteam {
    $steam = Get-SteamRootPath
    if (-not $steam) { return '' }
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf -PathType Leaf) {
        $text = [IO.File]::ReadAllText($vdf)
        foreach ($m in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
            $libs += $m.Groups[1].Value.Replace('\\', '\')
        }
    }
    foreach ($lib in $libs) {
        $acf = Join-Path $lib ('steamapps\appmanifest_' + $AppId + '.acf')
        if (-not (Test-Path -LiteralPath $acf -PathType Leaf)) { continue }
        $acfText = [IO.File]::ReadAllText($acf)
        $m = [regex]::Match($acfText, '"installdir"\s+"([^"]+)"')
        if (-not $m.Success) { continue }
        $dir = Join-Path $lib ('steamapps\common\' + $m.Groups[1].Value)
        if (Test-GameFolder $dir) { return $dir }
    }
    return ''
}

function Find-Game {
    if ($Game) {
        if (-not (Test-GameFolder $Game)) { Stop-With ('Psychonauts.exe was not found in "' + $Game + '".') }
        return $Game
    }
    $found = Find-GameViaSteam
    if ($found) { return $found }
    $pf = ${env:ProgramFiles(x86)}
    if ($pf) {
        $default = Join-Path $pf 'Steam\steamapps\common\Psychonauts'
        if (Test-GameFolder $default) { return $default }
    }
    if ($NoPrompt) {
        Stop-With 'Could not find Psychonauts. Pass the folder that contains Psychonauts.exe with -Game.'
    }
    Write-Host 'Could not find Psychonauts automatically.'
    Write-Host 'Choose the game folder (the one that contains Psychonauts.exe).'
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Choose the Psychonauts folder (the one that contains Psychonauts.exe)'
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Stop-With 'No folder chosen.' }
    if (-not (Test-GameFolder $dlg.SelectedPath)) {
        Stop-With ('Psychonauts.exe was not found in "' + $dlg.SelectedPath + '".')
    }
    return $dlg.SelectedPath
}

function Get-PeStamp([string]$Path) {
    $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $br = New-Object IO.BinaryReader($fs)
        if ($fs.Length -lt 0x40) { return $null }
        if ($br.ReadUInt16() -ne 0x5A4D) { return $null }
        $fs.Seek(0x3C, 'Begin') | Out-Null
        $pe = $br.ReadUInt32()
        if ($pe -gt ($fs.Length - 12)) { return $null }
        $fs.Seek($pe, 'Begin') | Out-Null
        if ($br.ReadUInt32() -ne 0x00004550) { return $null }
        $fs.Seek($pe + 8, 'Begin') | Out-Null
        return $br.ReadUInt32()
    } finally { $fs.Close() }
}

# Our d3d9.dll names psyvr.ini; another mod's (ReShade, ...) does not.
function Test-OurDll([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $ascii = 'psyvr.ini'
    $wide = ($ascii.ToCharArray() | ForEach-Object { [string]$_ + [char]0 }) -join ''
    return ($text.IndexOf($ascii, [StringComparison]::Ordinal) -ge 0) -or
           ($text.IndexOf($wide, [StringComparison]::Ordinal) -ge 0)
}

function Test-Running {
    $p = Get-Process -Name 'Psychonauts' -ErrorAction SilentlyContinue
    return [bool]$p
}

function Test-Writable([string]$Dir) {
    $probe = Join-Path $Dir ('.psyvr-write-test-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllText($probe, 'x')
        Remove-Item -LiteralPath $probe -Force
        return $true
    } catch { return $false }
}

function Restart-Elevated([string]$GameDir) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'),
                 '-Game', ('"' + $GameDir + '"'))
    if ($Uninstall) { $argList += '-Uninstall' }
    if ($Force) { $argList += '-Force' }
    if ($Source) { $argList += @('-Source', ('"' + $Source + '"')) }
    try {
        $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs -Wait -PassThru
        exit $p.ExitCode
    } catch {
        Stop-With 'The game folder is not writable. Right-click Install.bat and choose "Run as administrator".'
    }
}

function Read-Manifest([string]$GameDir) {
    $path = Join-Path $GameDir $ManifestName
    $result = @{ Exists = $false; Files = @(); Backup = ''; Astra = $false }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $result }
    $result.Exists = $true
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        if ($line -match '^file:\s*(.+?)\s*$') { $result.Files += $Matches[1] }
        elseif ($line -match '^backup:\s*(.+?)\s*$') { $result.Backup = $Matches[1] }
        elseif ($line -match '^astralathe:\s*yes\s*$') { $result.Astra = $true }
    }
    return $result
}

function Test-SafeName([string]$Name) {
    if (-not $Name) { return $false }
    if ($Name -ne (Split-Path -Leaf $Name)) { return $false }
    if ($Name -eq '.' -or $Name -eq '..') { return $false }
    if ($Protected -contains $Name.ToLowerInvariant()) { return $false }
    return $true
}

# The mod sets the game's resolution in DisplaySettings.ini: put back the
# player's own file, or remove one the mod created (it leaves a
# .psyvr-created mark then). A file with neither is the player's: kept.
function Restore-DisplaySettings([string]$GameDir) {
    $ds = Join-Path $GameDir 'DisplaySettings.ini'
    $bk = Join-Path $GameDir 'DisplaySettings.ini.psyvr-backup'
    $mk = Join-Path $GameDir 'DisplaySettings.ini.psyvr-created'
    if (Test-Path -LiteralPath $bk -PathType Leaf) {
        Move-Item -LiteralPath $bk -Destination $ds -Force
        Write-Host '  restored: DisplaySettings.ini (your resolution settings from before the mod)'
    } elseif (Test-Path -LiteralPath $mk -PathType Leaf) {
        if (Test-Path -LiteralPath $ds -PathType Leaf) { Remove-Item -LiteralPath $ds -Force }
        Write-Host '  removed: DisplaySettings.ini (the mod had created it; the game makes a new one)'
    }
    if (Test-Path -LiteralPath $mk -PathType Leaf) { Remove-Item -LiteralPath $mk -Force }
}

# ---------------------------------------------------------------------------

Write-Host 'Psychonauts VR mod'
Write-Host '=================='

$gameDir = Find-Game
$gameDir = $gameDir.TrimEnd('\')
Write-Host ('Game folder: ' + $gameDir)

if (Test-Running) { Stop-With 'The game (Psychonauts.exe) is running. Close it and run this again.' }

if (-not (Test-Writable $gameDir)) {
    if ($NoPrompt) { Stop-With ('The game folder is not writable: ' + $gameDir) }
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) { Stop-With ('The game folder is not writable: ' + $gameDir) }
    Write-Host 'The game folder needs administrator rights; asking Windows for them...'
    Restart-Elevated $gameDir
}

# ---------------------------------------------------------------- uninstall
if ($Uninstall) {
    Write-Host 'Removing the mod...'
    $mf = Read-Manifest $gameDir
    $backup = ''
    $names = @()
    $ours = $true
    if ($mf.Exists) {
        $names = $mf.Files
        $backup = $mf.Backup
    } else {
        Write-Host ('No ' + $ManifestName + ' found; using the known file names.')
        $d3d9 = Join-Path $gameDir 'd3d9.dll'
        if ((Test-Path -LiteralPath $d3d9 -PathType Leaf) -and (Test-OurDll $d3d9)) {
            $names = $KnownFiles
        } else {
            $ours = $false
            Write-Host 'd3d9.dll in the game folder is not this mod; no mod files are removed.'
        }
        if (Test-Path -LiteralPath (Join-Path $gameDir 'd3d9.dll.psyvr-backup') -PathType Leaf) {
            $backup = 'd3d9.dll.psyvr-backup'
        }
    }
    foreach ($n in $names) {
        if (-not (Test-SafeName $n)) { Write-Host ('  skipped (not allowed): ' + $n); continue }
        $p = Join-Path $gameDir $n
        if ($n -eq 'd3d9.dll' -and (Test-Path -LiteralPath $p -PathType Leaf) -and -not (Test-OurDll $p)) {
            # Another mod replaced ours since the install; leave its d3d9.dll alone.
            Write-Host '  kept: d3d9.dll (it is not this mod''s any more)'
            continue
        }
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            Remove-Item -LiteralPath $p -Force
            Write-Host ('  removed: ' + $n)
        }
    }
    if ($backup -and (Test-SafeName $backup)) {
        $bp = Join-Path $gameDir $backup
        $target = Join-Path $gameDir 'd3d9.dll'
        if ((Test-Path -LiteralPath $bp -PathType Leaf) -and -not (Test-Path -LiteralPath $target)) {
            Move-Item -LiteralPath $bp -Destination $target
            Write-Host ('  restored: d3d9.dll (from ' + $backup + ')')
        }
    }
    if ($ours) {
        $astra = Join-Path $gameDir $AstraDir
        if (Test-Path -LiteralPath $astra -PathType Container) {
            Remove-Item -LiteralPath $astra -Recurse -Force
            Write-Host ('  removed: ' + $AstraDir)
        }
        Restore-DisplaySettings $gameDir
    }
    $manifestPath = Join-Path $gameDir $ManifestName
    if (Test-Path -LiteralPath $manifestPath) { Remove-Item -LiteralPath $manifestPath -Force }
    Write-Host ''
    Write-Host 'Done. psyvr.ini and psyvr.log were kept (delete them yourself if you want).'
    Write-Host 'Psychonauts.exe and your saves were never touched.'
    Wait-Enter
    exit 0
}

# ------------------------------------------------------------------ install
$packageRoot = Split-Path $PSScriptRoot
if (-not $Source) { $Source = Join-Path $packageRoot 'Game files' }
if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
    Stop-With ('The "Game files" folder was not found at "' + $Source + '". Unzip the whole package first.')
}
$payload = @(Get-ChildItem -LiteralPath $Source -File)
if ($payload.Count -eq 0) { Stop-With ('"' + $Source + '" is empty.') }

$stamp = Get-PeStamp (Join-Path $gameDir 'Psychonauts.exe')
if ($null -eq $stamp -or [uint32]$stamp -ne $SupportedStamp) {
    $shown = 'not a valid program'
    if ($null -ne $stamp) { $shown = ('0x{0:X8}' -f [uint32]$stamp) }
    Write-Host ''
    Write-Host ('WARNING: this Psychonauts.exe is not the supported Steam build (build stamp ' + $shown + ',')
    Write-Host '         expected 0x568C5772). Only the Steam version is supported. On another'
    Write-Host '         build the mod skips every game patch whose code differs, so VR may run'
    Write-Host '         with features missing, or not at all.'
    $go = $Force.IsPresent
    if (-not $go) {
        if ($NoPrompt) { Stop-With 'Unsupported Psychonauts.exe; nothing was installed. Use -Force to install anyway.' }
        $answer = Read-Host 'Install anyway? (y/N)'
        $go = ($answer -match '^\s*[yY]')
    }
    if (-not $go) { Stop-With 'Cancelled; nothing was installed.' }
}

$oldManifest = Read-Manifest $gameDir
$backupName = ''
$d3d9Path = Join-Path $gameDir 'd3d9.dll'
if ((Test-Path -LiteralPath $d3d9Path -PathType Leaf) -and -not (Test-OurDll $d3d9Path)) {
    $backupName = 'd3d9.dll.psyvr-backup'
    if (Test-Path -LiteralPath (Join-Path $gameDir $backupName)) {
        $backupName = 'd3d9.dll.psyvr-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
    }
    Move-Item -LiteralPath $d3d9Path -Destination (Join-Path $gameDir $backupName)
    Write-Host ('Another d3d9.dll (another mod, e.g. ReShade) was in the game folder; kept as ' + $backupName)
} elseif ($oldManifest.Backup -and (Test-Path -LiteralPath (Join-Path $gameDir $oldManifest.Backup) -PathType Leaf)) {
    # Reinstall: keep remembering the earlier backup so Uninstall can still restore it.
    $backupName = $oldManifest.Backup
}

Write-Host 'Installing files...'
$installed = @()
foreach ($f in $payload) {
    if ($Protected -contains $f.Name.ToLowerInvariant()) { continue }
    $dst = Join-Path $gameDir $f.Name
    Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
    try { Unblock-File -LiteralPath $dst } catch { }
    $installed += $f.Name
    Write-Host ('  ' + $f.Name)
}

# Optional: the Astralathe part, only where Astralathe is installed.
$astraInstalled = $false
$astraSrc = Join-Path $packageRoot 'Optional\PsyVR'
if ((Test-Path -LiteralPath (Join-Path $gameDir 'ModResource') -PathType Container) -and
    (Test-Path -LiteralPath $astraSrc -PathType Container)) {
    $astraDst = Join-Path $gameDir $AstraDir
    New-Item -ItemType Directory -Force -Path $astraDst | Out-Null
    Copy-Item -Path (Join-Path $astraSrc '*') -Destination $astraDst -Recurse -Force
    $astraInstalled = $true
    Write-Host ('  ' + $AstraDir + ' (Astralathe is installed; optional)')
}

$version = ''
$verFile = Join-Path $packageRoot 'VERSION.txt'
if (Test-Path -LiteralPath $verFile -PathType Leaf) { $version = ([IO.File]::ReadAllText($verFile)).Trim() }
$lines = @('Psychonauts VR mod - installed files (used by Uninstall.bat)')
if ($version) { $lines += ('version: ' + $version) } else { $lines += 'version: unknown' }
$lines += ('date: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
if ($backupName) { $lines += ('backup: ' + $backupName) }
if ($astraInstalled) { $lines += 'astralathe: yes' }
foreach ($n in $installed) { $lines += ('file: ' + $n) }
Set-Content -LiteralPath (Join-Path $gameDir $ManifestName) -Value $lines -Encoding ASCII

# --------------------------------------------------------------- pre-flight
Write-Host ''
Write-Host 'Checking the rest of the set-up:'
$vk = Join-Path $env:WINDIR 'SysWOW64\vulkan-1.dll'
if (Test-Path -LiteralPath $vk -PathType Leaf) {
    Write-Host '  OK       Vulkan (32-bit) is present.'
} else {
    Write-Host '  WARNING  32-bit Vulkan (SysWOW64\vulkan-1.dll) was not found. Update your graphics driver.'
}
$vdJson = Join-Path $env:ProgramW6432 'Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json'
$vdOk = Test-Path -LiteralPath $vdJson -PathType Leaf
if ($vdOk) {
    Write-Host '  OK       Virtual Desktop Streamer (32-bit OpenXR) is installed.'
} else {
    Write-Host '  WARNING  Virtual Desktop Streamer was not found. Install the Virtual Desktop Streamer'
    Write-Host '           app on this PC (from the Virtual Desktop website) so the mod has an OpenXR runtime.'
}
$active = ''
try {
    $active = [string](Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1' -Name 'ActiveRuntime' -ErrorAction Stop).ActiveRuntime
} catch { }
if ($active) {
    if ($active -match 'virtualdesktop') {
        Write-Host '  OK       The active 32-bit OpenXR runtime is Virtual Desktop.'
    } else {
        Write-Host ('  WARNING  Another 32-bit OpenXR runtime is set as active: ' + $active)
        Write-Host '           The mod will use that one instead of Virtual Desktop. Only Quest 3 over'
        Write-Host '           Virtual Desktop has been tested so far.'
    }
}
if (Test-Path -LiteralPath (Join-Path $gameDir 'ModResource') -PathType Container) {
    Write-Host '  OK       Astralathe found (optional; not needed to play in VR).'
} else {
    Write-Host '  OK       No Astralathe (optional; not needed to play in VR).'
}

Write-Host ''
Write-Host 'Installed. How to play:'
Write-Host '  1. Connect your Quest 3 to this PC with Virtual Desktop.'
Write-Host '  2. Start Psychonauts from Steam as usual, or run'
Write-Host '     "Play Psychonauts VR.bat" from the game folder.'
Write-Host 'Settings are written to psyvr.ini in the game folder on the first run.'
Write-Host 'To remove the mod, run Uninstall.bat.'
Wait-Enter
exit 0
