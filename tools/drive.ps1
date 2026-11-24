# Drives a windowed Psychonauts.exe for diagnostics: launch, screenshot, keys.
#   . .\drive.ps1 ; Start-Psy ; Shot a.png ; Key ENTER ; Stop-Psy
# Keys go through keybd_event as virtual-key events.
$ErrorActionPreference = 'Stop'

# --- Configuration ------------------------------------------------------------
# Every machine-specific path lives in this block. Each can be overridden with
# an environment variable set before the script is dot-sourced.
#   PSY_GAME_DIR       game folder (the one with Psychonauts.exe)
#   PSY_REPO_DIR       this repository (default: the parent of tools\)
#   PSY_WORK_DIR       scratch folder for screenshots, test saves, settings, spots
#   PSY_XR_SIM_JSON    manifest of a 32-bit OpenXR runtime used for the test
#                      runs (e.g. a 32-bit build of OpenXR-Simulator)
#   PSY_GHIDRA_DIR     Ghidra install folder (only for the Ghidra function)
#   PSY_GHIDRA_PROJECT Ghidra project folder, and PSY_GHIDRA_NAME the project
#                      name inside it (default: psychonauts)
function Get-Setting([string]$Name, [string]$Default) {
    $v = [Environment]::GetEnvironmentVariable($Name)
    if ($v) { $v } else { $Default }
}
$RepoDir       = Get-Setting 'PSY_REPO_DIR' (Split-Path $PSScriptRoot -Parent)
$GameDir       = Get-Setting 'PSY_GAME_DIR' "${env:ProgramFiles(x86)}\Steam\steamapps\common\Psychonauts"
$WorkDir       = Get-Setting 'PSY_WORK_DIR' (Join-Path $env:USERPROFILE 'tools\work')
$ShotDir       = Join-Path $WorkDir 'shots'
$SaveStore     = Join-Path $WorkDir 'psy_saves'
$SettingsStore = Join-Path $WorkDir 'settings'
$SpotsFile     = Join-Path $WorkDir 'spots.json'
$SpotsDir      = Join-Path $WorkDir 'spots'
$SimJson       = Get-Setting 'PSY_XR_SIM_JSON' (Join-Path $env:USERPROFILE 'tools\openxr-sim\src\bin\openxr_simulator.json')
$GhidraDir     = Get-Setting 'PSY_GHIDRA_DIR' (Join-Path $env:USERPROFILE 'tools\ghidra\ghidra_12.1.4_PUBLIC')
$GhidraProject = Get-Setting 'PSY_GHIDRA_PROJECT' (Join-Path $env:USERPROFILE 'tools\gproj')
$GhidraName    = Get-Setting 'PSY_GHIDRA_NAME' 'psychonauts'
# -----------------------------------------------------------------------------
$GameExe = 'Psychonauts'      # process name, without .exe
New-Item -ItemType Directory -Force $ShotDir | Out-Null

Add-Type -AssemblyName System.Drawing
if (-not ('PsyDrive.W' -as [type])) {
Add-Type -Namespace PsyDrive -Name W -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool GetClientRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(System.IntPtr h, ref System.Drawing.Point p);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
'@ -ReferencedAssemblies System.Drawing
}

# The game window by its class ("Psychonauts"): the process's MainWindowHandle
# can be the OpenXR simulator's preview window, which lives in the same process.
if (-not ('PsyDrive.FW' -as [type])) {
Add-Type -Namespace PsyDrive -Name FW -MemberDefinition '[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern System.IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, System.IntPtr pid);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(System.IntPtr h);
[DllImport("user32.dll")] public static extern void SwitchToThisWindow(System.IntPtr h, bool alt);'
}
function Get-PsyWindow {
    if (-not (Get-Process -Name $GameExe -ErrorAction SilentlyContinue)) { return [IntPtr]::Zero }
    $h = [PsyDrive.FW]::FindWindowW('Psychonauts', $null)
    if ($h -ne [IntPtr]::Zero) { return $h }
    $p = Get-Process -Name $GameExe -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { return $p.MainWindowHandle } else { return [IntPtr]::Zero }
}

# Launches Psychonauts.exe directly (it is not SteamStub-wrapped, so no
# unpacked copy is needed).
function Start-Psy {
    Start-Process "$GameDir\$GameExe.exe" -WorkingDirectory $GameDir | Out-Null
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        $h = Get-PsyWindow
        if ($h -ne [IntPtr]::Zero) { Focus-Psy; return "window $h" }
    }
    return 'no window'
}

function Focus-Psy {
    # Plain SetForegroundWindow. Do not attach to the game thread's input to
    # force it: the OpenXR simulator's window is on that thread, and that hung
    # its xrEndFrame (2026-10-21). With the simulator's preview in front, keys
    # sent in gameplay may not reach the game.
    $h = Get-PsyWindow
    if ($h -ne [IntPtr]::Zero) { [PsyDrive.W]::ShowWindow($h, 9) | Out-Null; [PsyDrive.W]::SetForegroundWindow($h) | Out-Null }
}

function Shot([string]$name = 'shot.png', [int]$scaleTo = 960) {
    $h = Get-PsyWindow
    if ($h -eq [IntPtr]::Zero) { return 'no window' }
    $r = New-Object PsyDrive.W+RECT
    [PsyDrive.W]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object System.Drawing.Point 0, 0
    [PsyDrive.W]::ClientToScreen($h, [ref]$pt) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return 'empty client' }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($pt, [System.Drawing.Point]::Empty, (New-Object System.Drawing.Size $w, $hh))
    $sw = [Math]::Min($scaleTo, $w); $sh = [int]($hh * $sw / $w)
    $small = New-Object System.Drawing.Bitmap $bmp, $sw, $sh
    $path = Join-Path $ShotDir $name
    $small.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose(); $small.Dispose()
    return $path
}

$VK = @{ ENTER = 0x0D; ESC = 0x1B; SPACE = 0x20; UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27; F9 = 0x78; INSERT = 0x2D; W = 0x57; A = 0x41; S = 0x53; D = 0x44; E = 0x45 }

function Key([string]$k, [int]$holdMs = 200) {
    # VK events (keybd_event) register reliably. A game ignores keys for a
    # moment after it is activated, hence the pause after focusing.
    if ([PsyDrive.W]::GetForegroundWindow() -ne (Get-PsyWindow)) { Unlock-Fg; Focus-Psy; Start-Sleep -Milliseconds 500 }
    $vk = [byte]$VK[$k]
    $scan = [byte][PsyDrive.W]::MapVirtualKey($vk, 0)
    $ext = if ($k -in 'UP','DOWN','LEFT','RIGHT') { 1 } else { 0 }
    [PsyDrive.M]::keybd_event($vk, $scan, $ext, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $holdMs
    [PsyDrive.M]::keybd_event($vk, $scan, ($ext -bor 2), [UIntPtr]::Zero)
}

function Stop-Psy {
    Get-Process -Name $GameExe -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    # The test-only Continue mod never stays installed after a test.
    Remove-Item (Join-Path $GameDir "ModResource\PsyVRTest") -Recurse -Force -ErrorAction SilentlyContinue
    # And the player's own psyvr.ini comes back (a test ini points at the simulator),
    # and their profile file (a test one points Continue at the test slot).
    Restore-UserIni | Out-Null
    Restore-PlayerProfile | Out-Null
}

if (-not ('PsyDrive.M' -as [type])) {
Add-Type -Namespace PsyDrive -Name M -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint data, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
'@
}

# Alt tap: lets SetForegroundWindow succeed from a background process.
function Unlock-Fg { [PsyDrive.M]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [PsyDrive.M]::keybd_event(0x12,0,2,[UIntPtr]::Zero) }

# Click at a position given in screenshot coordinates (960-wide shots of the
# client; $shotScale = client width / 960).
function Click([int]$sx, [int]$sy, [double]$shotScale = (1280.0 / 960.0)) {
    Focus-Psy
    $h = Get-PsyWindow
    $pt = New-Object System.Drawing.Point ([int]($sx * $shotScale)), ([int]($sy * $shotScale))
    [PsyDrive.W]::ClientToScreen($h, [ref]$pt) | Out-Null
    [PsyDrive.M]::SetCursorPos($pt.X, $pt.Y) | Out-Null
    Start-Sleep -Milliseconds 120
    [PsyDrive.M]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [PsyDrive.M]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero)
}

function Capture { Set-Content -Path "$GameDir\capture.flag" -Value 1 }

# Scripted gamepad input (the mod's pad_script.txt): one "<seconds> [tokens]"
# line per step, e.g. Pad-Script @('2 ly=1', '1', '1.5 rx=1') -Wait. -Wait
# sleeps the script's total seconds plus 1.
function Pad-Script([string[]]$Lines, [switch]$Wait) {
    [IO.File]::WriteAllText("$GameDir\pad_script.txt", (($Lines -join "`r`n") + "`r`n"), [Text.Encoding]::ASCII)
    if ($Wait) {
        $total = 0.0
        foreach ($l in $Lines) {
            $t = ($l -replace '#.*', '').Trim() -split '\s+' | Select-Object -First 1
            $v = 0.0
            if ($t -and [double]::TryParse($t, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$v)) { $total += $v }
        }
        Start-Sleep -Milliseconds ([int](($total + 1) * 1000))
    }
}

# Stops a playing pad script at the mod's next poll.
function Pad-Stop { Set-Content -Path "$GameDir\pad_script_stop.flag" -Value 1 }

if (-not ('PsyDrive.P' -as [type])) {
Add-Type -Namespace PsyDrive -Name P -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PrintWindow(System.IntPtr h, System.IntPtr hdc, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
'@
}

# Every visible top-level window of the game (and an in-process runtime's
# preview window), each captured on its own with PrintWindow so overlapping
# windows do not spoil the picture. Returns "title -> path".
# -LeftEye keeps only the left half (one eye of the side-by-side game window;
# other windows are left whole): a quarter of the pixels, for cheap looks.
function Shot-All([string]$prefix = 'win', [int]$scaleTo = 960, [switch]$LeftEye) {
    $pids = @(Get-Process -Name $GameExe -ErrorAction SilentlyContinue | ForEach-Object Id)
    $found = New-Object System.Collections.ArrayList
    $cb = [PsyDrive.P+EnumProc]{ param($h, $l)
        $p = 0; [PsyDrive.P]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
        if ($pids -contains $p -and [PsyDrive.P]::IsWindowVisible($h)) { [void]$found.Add($h) }
        $true }
    [PsyDrive.P]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    $i = 0
    foreach ($h in $found) {
        $t = New-Object Text.StringBuilder 256; [PsyDrive.P]::GetWindowTextW($h, $t, 256) | Out-Null
        $r = New-Object PsyDrive.P+RECT; [PsyDrive.P]::GetWindowRect($h, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        if ($w -le 0 -or $hh -le 0) { continue }
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [PsyDrive.P]::PrintWindow($h, $dc, 2) | Out-Null
        $g.ReleaseHdc($dc)
        if ($LeftEye -and $t.ToString() -like 'Psychonauts*') {
            $half = $bmp.Clone((New-Object System.Drawing.Rectangle 0, 0, ([int]($w / 2)), $hh), $bmp.PixelFormat)
            $g.Dispose(); $bmp.Dispose()
            $bmp = $half; $g = [System.Drawing.Graphics]::FromImage($bmp); $w = $bmp.Width
        }
        $sw = [Math]::Min($scaleTo, $w); $sh = [int]($hh * $sw / $w)
        $small = New-Object System.Drawing.Bitmap $bmp, $sw, $sh
        $path = Join-Path $ShotDir ("{0}{1}.png" -f $prefix, $i)
        $small.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose(); $small.Dispose()
        "$($t.ToString()) ${w}x$hh -> $path"
        $i++
    }
}

# Test runs never touch the player's psyvr.ini: the test settings go to
# psyvr.test.ini and the game is started with PSYVR_INI pointing at it
# (config.cpp), so a game launched any other way (Steam, the headset) reads
# the player's own file and runtime even if a test was not cleaned up.
# Save-UserIni still keeps a copy of the player's file; Restore-UserIni also
# puts it back if an older harness left a test ini (marked by its first line)
# in its place.
$TestIni = "$GameDir\psyvr.test.ini"
$TestIniMark = '; psyvr test run (tools\drive.ps1): not the player''s settings'
function Save-UserIni {
    $ini = "$GameDir\psyvr.ini"
    if (-not (Test-Path $ini)) { return }
    if ((Get-Content $ini -TotalCount 1) -eq $TestIniMark) { return }
    New-Item -ItemType Directory -Force $SettingsStore | Out-Null
    Copy-Item $ini "$SettingsStore\psyvr.user.ini" -Force
    "settings: the player's psyvr.ini kept in $SettingsStore"
}
function Restore-UserIni {
    $ini = "$GameDir\psyvr.ini"
    if ((Test-Path $ini) -and (Get-Content $ini -TotalCount 1) -eq $TestIniMark) { Remove-Item $ini -Force }
    if ((-not (Test-Path $ini)) -and (Test-Path "$SettingsStore\psyvr.user.ini")) {
        Copy-Item "$SettingsStore\psyvr.user.ini" $ini -Force
        "settings: the player's psyvr.ini is back"
    }
}

# One VR test cycle against the 32-bit OpenXR simulator: install the build,
# start on the simulator (extra ini lines via -Ini), load a test save (-Save,
# default gpc; or stay on the main menu with -StopAtMenu), settle, screenshot
# every window (-ShotWidth, -LeftEye). Leaves the game running; end with Clean-Game.
function Run-Vr([string[]]$Ini = @(), [int]$Settle = 8, [switch]$StopAtMenu, [string]$Save = 'gpc',
                [int]$ShotWidth = 1600, [switch]$LeftEye) {
    if ($StopAtMenu) { Start-TestGame -Ini $Ini -Force } else { Start-TestGame -Ini $Ini -Save $Save -Force }
    if ($StopAtMenu) { $ok = Wait-Log $AstraLog 'LOADING LEVEL st\.STMU' 300 }
    else { $ok = Wait-Status (Get-SaveLevel $Save) 300 }
    if ($null -eq $ok) { return 'Run-Vr: the level never loaded (see psyvr.log / astralathe.log)' }
    Start-Sleep $Settle
    Shot-All vr $ShotWidth -LeftEye:$LeftEye
}

# Rebuilds only psyvr_logic.dll, copies it into the game folder and, when the
# game runs (started by Start-TestGame / Quick-Vr: [debug] hot_reload=1),
# waits for it to be swapped in (psyvr.log: "logic: reloaded in N ms", or
# "reload failed"). Returns {Ok, Stage, BuildSec, ReloadMs, TotalSec, Line}.
# A build error is printed and returned (Stage = 'build') without touching
# the game. -NoWait: copy only.
function Push-Logic([int]$TimeoutSec = 20, [switch]$NoWait) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = @(cmd /c "$RepoDir\build.bat logic" 2>&1 | ForEach-Object { "$_" })
    $buildSec = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    if ($LASTEXITCODE -ne 0) {
        $errs = @($out | Where-Object { $_ -match 'error|FAILED' })
        $errs | ForEach-Object { Write-Host $_ }
        return [pscustomobject]@{ Ok = $false; Stage = 'build'; BuildSec = $buildSec; ReloadMs = $null
            TotalSec = $buildSec; Line = ($errs -join "`n") }
    }
    $running = [bool](Get-Process -Name $GameExe -ErrorAction SilentlyContinue)
    $from = (Read-LogLines $PsyLog).Count
    Copy-Item "$RepoDir\dist\psyvr_logic.dll" "$GameDir\psyvr_logic.dll" -Force
    if ($NoWait -or -not $running) {
        return [pscustomobject]@{ Ok = $true; Stage = $(if ($running) { 'copied' } else { 'copied, game not running' })
            BuildSec = $buildSec; ReloadMs = $null; TotalSec = [math]::Round($sw.Elapsed.TotalSeconds, 1); Line = $null }
    }
    # From the last (empty) element of the split: the log ends with a
    # newline, so the next line lands there. A new log (a restart) has fewer
    # lines: then read it from the top.
    if ((Read-LogLines $PsyLog).Count -lt $from) { $from = 0 }
    $line = Wait-Log $PsyLog 'logic: (reloaded in|reload failed)|logic: host does not match' $TimeoutSec ([math]::Max(0, $from - 1))
    $ok = $line -match 'reloaded in (\d+) ms'
    $ms = if ($ok) { [int]$Matches[1] } else { $null }
    if (-not $line) { $line = 'no reload line (is [debug] hot_reload = 1?)' }
    [pscustomobject]@{ Ok = $ok; Stage = $(if ($ok) { 'reloaded' } else { 'reload' }); BuildSec = $buildSec; ReloadMs = $ms
        TotalSec = [math]::Round($sw.Elapsed.TotalSeconds, 1); Line = $line }
}

# --- control channel ([debug] control_pipe = 1; Run-Vr sets it) -------------

# Sends one command to the running game over the named pipe \\.\pipe\psyvr and
# returns the reply lines (the final "." excluded). The first line is
# `ok ...` or `error ...`. Commands: ping, status, trace n, shot name,
# drawlog name, set section key value.
function Psy-Cmd([string]$Command) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'psyvr', [System.IO.Pipes.PipeDirection]::InOut)
    try {
        try { $pipe.Connect(3000) }
        catch { throw "Psy-Cmd: cannot connect to the psyvr pipe - is [debug] control_pipe = 1 and the game running? ($($_.Exception.Message))" }
        $bytes = [Text.Encoding]::ASCII.GetBytes($Command + "`n")
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
        $reader = New-Object System.IO.StreamReader($pipe, [Text.Encoding]::ASCII)
        $out = @()
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line -or $line -eq '.') { break }
            $out += $line
        }
        return $out
    }
    finally { $pipe.Dispose() }
}

# One screenshot of what the eyes get: <GameDir>\psyvr_shots\<name>_L.png and
# _R.png, plus <name>.trace.txt (the traced frame's psyvr.log lines, cut out
# between the `shot <name>: trace begin/end` markers), plus <name>.draws.jsonl
# (the same frame's draw log). Returns the four paths. -EyesOnly: just the two
# PNGs (no trace, no draw log; much faster), returns their two paths.
function Psy-Shot([string]$Name, [switch]$EyesOnly) {
    if ($EyesOnly) {
        $reply = @(Psy-Cmd "shot $Name eyes")
        if ($reply.Count -lt 3 -or $reply[0] -notlike 'ok*') { throw "Psy-Shot: $($reply -join ' | ')" }
        return @($reply[1], $reply[2])
    }
    $reply = @(Psy-Cmd "shot $Name")
    if ($reply.Count -lt 3 -or $reply[0] -notlike 'ok*') { throw "Psy-Shot: $($reply -join ' | ')" }
    $log = @(Get-Content "$GameDir\psyvr.log" -Encoding UTF8 -ErrorAction SilentlyContinue)
    $begin = -1; $end = -1
    for ($i = 0; $i -lt $log.Count; $i++) {
        if ($log[$i] -like "*shot ${Name}: trace begin*") { $begin = $i; $end = -1 }
        elseif ($begin -ge 0 -and $end -lt 0 -and $log[$i] -like "*shot ${Name}: trace end*") { $end = $i }
    }
    $trace = "$GameDir\psyvr_shots\$Name.trace.txt"
    if ($begin -ge 0 -and $end -gt $begin) { $cut = @($log[$begin..$end]) } else { $cut = @() }
    Set-Content $trace -Encoding UTF8 -Value $cut
    $paths = @($reply[1], $reply[2], $trace)
    if ($reply.Count -ge 4) { $paths += $reply[3] }     # <name>.draws.jsonl
    return $paths
}

# The next frame's draw log, one JSON object per draw, without screenshots:
# <GameDir>\psyvr_shots\<name>.draws.jsonl (summarise it with
# `python -I tools\drawlog_summary.py <file>`). Returns the file's path.
function Psy-DrawLog([string]$Name) {
    $reply = @(Psy-Cmd "drawlog $Name")
    if ($reply.Count -lt 2 -or $reply[0] -notlike 'ok*') { throw "Psy-DrawLog: $($reply -join ' | ')" }
    return $reply[1]
}

# Draws in the most recent logged frame (logged every 300 frames), or -1.
function Get-Draws {
    $l = Get-Content "$GameDir\psyvr.log" -ErrorAction SilentlyContinue | Select-String "draws in the last frame" | Select-Object -Last 1
    if ("$l" -match ': (\d+) draws') { return [int]$matches[1] } else { return -1 }
}

# Put the game folder back to stock (mod files, logs); the player's own
# psyvr.ini is put back (Restore-UserIni), a test ini removed.
function Clean-Game {
    Stop-Psy
    cmd /c "$RepoDir\uninstall.bat" | Out-Null
    Restore-UserIni
    Remove-Item $TestIni, "$GameDir\psyvr.log", "$GameDir\psyvr.log.prev", "$GameDir\trace.flag", "$GameDir\pad_script.txt", "$GameDir\pad_script_stop.flag" -ErrorAction SilentlyContinue
    Remove-Item "$GameDir\psyvr_shots" -Recurse -Force -ErrorAction SilentlyContinue
}

# Runs a generic Ghidra script from tools\ against an analysed Psychonauts
# project: Ghidra DumpFuncs.java out.txt 5DFBE0 5F39F0. Arguments must not
# contain spaces or parentheses (analyzeHeadless.bat).
# The project ($GhidraProject, $GhidraName; see the configuration block) is
# your own analysis of Psychonauts.exe; none is shipped.
function Ghidra([string]$script, [string[]]$scriptArgs) {
    & "$GhidraDir\support\analyzeHeadless.bat" $GhidraProject $GhidraName -process 'Psychonauts.exe' -noanalysis `
        -scriptPath "$RepoDir\tools" -postScript $script @scriptArgs 2>&1 |
        Select-String 'ERROR|Exception' | Select-Object -First 5
}

# Named test saves (docs/SAVES.md). Pristine copies live in $SaveStore\<name>\SavedGame;
# Restore-TestSave copies one into the TEST SLOT (profile 2, slot 5, kept for
# tests; the player's saves are slots 0-4 and 6) and makes it the newest save,
# so the test mod's Continue loads it. An autosave during a test only changes
# the test slot. Level = what astralathe.log says it loads ("LOADING LEVEL x").
$TestSlot = 'profiles\profile 2\SavedGame5'
$TestSaves = [ordered]@{
    gpc       = @{ Level = 'ca.CAGP'; About = 'Whispering Rock - GPC & Wilderness: the default, smoke test, stereo checks' }
    reception = @{ Level = 'ca.CARE'; About = 'Whispering Rock - Reception Area: outdoors; god mode rendering, culling' }
    circus    = @{ Level = 'mc.MCTC'; About = 'Meat Circus - Tent City, one enemy nearby: targeting and player actions' }
    lake      = @{ Level = 'ca.CABH'; About = 'Whispering Rock - Boathouse and Beach, at the lake: water reflections' }
}
function Restore-TestSave([string]$Name = 'gpc') {
    if (-not $TestSaves.Contains($Name)) { throw "unknown test save '$Name' (known: $($TestSaves.Keys -join ', '))" }
    $src = Join-Path $SaveStore "$Name\SavedGame"
    if (-not (Test-Path $src)) { throw "test save copy missing: $src" }
    $dst = Join-Path $GameDir $TestSlot
    Copy-Item $src $dst -Force
    (Get-Item $dst).LastWriteTime = Get-Date
    Set-TestProfile
}

# Continue loads the slot the profile file names, not the newest file. The
# 57-byte "Profile 2- Raz": byte 0x12 = last slot used, bytes 0x32..0x38 =
# slot 0..6 in use (a slot marked unused is reset to an empty file). For a test
# the player's file is kept in $ProfileKeep and the copy in the game points at
# the test slot; Stop-Psy / Clean-Game put the player's file back.
$ProfileFile = 'profiles\profile 2\Profile 2- Raz'
$ProfileKeep = Join-Path $SaveStore 'profile2_pretest'
function Set-TestProfile {
    $path = Join-Path $GameDir $ProfileFile
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -ne 57) { throw "Set-TestProfile: $path is $($b.Length) bytes, expected 57 (layout unknown)" }
    if (-not (Test-Path $ProfileKeep)) { Copy-Item $path $ProfileKeep }
    $slot = [int]($TestSlot -replace '^.*SavedGame', '')
    $b[0x12] = $slot
    $b[0x32 + $slot] = 1
    [IO.File]::WriteAllBytes($path, $b)
}
function Restore-PlayerProfile {
    if (Test-Path $ProfileKeep) {
        Copy-Item $ProfileKeep (Join-Path $GameDir $ProfileFile) -Force
        Remove-Item $ProfileKeep -Force
        "profile: the player's profile file is back"
    }
}
# The level a test save loads, short form for Wait-Status (CAGP) or long (ca.CAGP).
function Get-SaveLevel([string]$Name = 'gpc', [switch]$Long) {
    $l = $TestSaves[$Name].Level
    if ($Long) { $l } else { $l -replace '^.*\.', '' }
}

# --- test sessions: launch, wait on log lines, measure the eyes --------------
# No keystrokes reach the game here: the intro videos play out (the menu level
# loads ~25 s after launch) and the Astralathe test mod picks Continue.

$AstraLog = Join-Path $GameDir 'astralathe.log'
$PsyLog = Join-Path $GameDir 'psyvr.log'

# Lines of a log file the game may be writing (shared read); @() if missing.
function Read-LogLines([string]$Path) {
    if (-not (Test-Path $Path)) { return @() }
    try {
        $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite,Delete')
        $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
        try { $text = $sr.ReadToEnd() } finally { $sr.Dispose() }
        return @($text -split "`r?`n")
    }
    catch { return @() }
}

# Waits until a line of $Path (from line $FromLine on) matches the regex
# $Pattern; returns that line, or $null on timeout or when the game exits.
function Wait-Log([string]$Path, [string]$Pattern, [int]$TimeoutSec = 60, [int]$FromLine = 0) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        $lines = Read-LogLines $Path
        for ($i = $FromLine; $i -lt $lines.Count; $i++) { if ($lines[$i] -match $Pattern) { return $lines[$i] } }
        if (-not (Get-Process -Name $GameExe -ErrorAction SilentlyContinue)) { return $null }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

# Polls the control pipe's `status` until the game reports $Level (its short
# name, e.g. CAGP); returns the status lines, or $null on timeout.
function Wait-Status([string]$Level, [int]$TimeoutSec = 60) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        try { $s = @(Psy-Cmd status) } catch { $s = @() }
        if (($s -join ' ') -match "level $Level\b") { return $s }
        if (-not (Get-Process -Name $GameExe -ErrorAction SilentlyContinue)) { return $null }
        Start-Sleep 1
    }
    return $null
}

# Installs the build and starts the game on the simulator with a test ini
# ($Ini: extra lines, starting in [vr]; a [debug] section is merged).
# -Save <name>: the named test save goes into the test slot and the Astralathe
# test mod picks Continue on the main menu, so the game goes straight to that
# save's level (-Continue alone = -Save gpc). Without either: the main menu.
# Refuses when the game is already running, unless -Force (then kills it).
function Start-TestGame([string[]]$Ini = @(), [switch]$Continue, [string]$Save = '', [switch]$Force) {
    if ($Continue -and -not $Save) { $Save = 'gpc' }
    if ($Save) { $Continue = $true; if (-not $TestSaves.Contains($Save)) { throw "unknown test save '$Save'" } }
    if (Get-Process -Name $GameExe -ErrorAction SilentlyContinue) {
        if (-not $Force) { throw "Start-TestGame: the game is already running (use -Force to end it)" }
    }
    Stop-Psy
    cmd /c "$RepoDir\install.bat" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "install.bat failed (exit $LASTEXITCODE)" }
    Save-UserIni | Out-Null
    $mod = Join-Path $GameDir "ModResource\PsyVRTest"
    Remove-Item $mod -Recurse -Force -ErrorAction SilentlyContinue
    if ($Continue) {
        if (-not (Test-Path (Join-Path $GameDir 'dsound.dll'))) { throw "Start-TestGame -Continue: Astralathe is not installed" }
        New-Item -ItemType Directory -Force (Join-Path $GameDir 'ModResource') | Out-Null
        Copy-Item "$RepoDir\tools\astralathe_test_mod\PsyVRTest" (Join-Path $GameDir "ModResource") -Recurse -Force
        Restore-TestSave $Save
    }
    # -Ini's [vr] keys go into the one [vr] section: of a repeated section
    # only the first is read, so a second [vr] was ignored.
    $vrKeys = @(); $other = @(); $sec = ''
    foreach ($l in $Ini) {
        if ($l -match '^\s*\[(.+)\]\s*$') { $sec = $matches[1] }
        if ($sec -ieq 'vr') { if ($l -notmatch '^\s*\[') { $vrKeys += $l } }
        else { $other += $l }
    }
    $lines = @($TestIniMark, '[vr]', "xr_runtime_json=$SimJson") + $vrKeys + $other
    # skip_fullscreen_passes=1: the game's full-screen effect passes stay off
    # in tests, as the user plays (they block sprites the mod cannot place in
    # the world, and shots taken with them on are not comparable).
    $debugLines = @('control_pipe=1', 'hang_exit_seconds=20', 'hot_reload=1', 'skip_fullscreen_passes=1')
    $at = [Array]::IndexOf($lines, '[debug]')
    if ($at -ge 0) {
        $rest = if ($at + 1 -lt $lines.Count) { @($lines[($at + 1)..($lines.Count - 1)]) } else { @() }
        $lines = @($lines[0..$at]) + $debugLines + $rest
    }
    else { $lines = $lines + '[debug]' + $debugLines }
    Set-Content $TestIni -Encoding Unicode -Value ($lines -join "`r`n")
    Set-SimDisplaySettings
    # Fresh logs, so waits only see this launch.
    Remove-Item $AstraLog -Force -ErrorAction SilentlyContinue
    # Only the game started here gets the test settings (Start-Process
    # inherits this session's environment).
    $env:PSYVR_INI = $TestIni
    try { Start-Psy | Out-Null }
    finally { Remove-Item Env:PSYVR_INI -ErrorAction SilentlyContinue }
}

# The game reads DisplaySettings.ini before the mod's own sync to the VR back
# buffer (dllmain.cpp, at Direct3DCreate9), so the player's headset size
# (4224x2304) left there by Clean-Game sized the game's effect targets on the
# next test launch: they no longer matched the simulator's 2560x1400 back
# buffer and went mono (docs/findings.md 2026-11-16). Written before launch,
# with the same backup the mod keeps (.psyvr-backup, put back by
# uninstall.bat), unless the mod created the file (.psyvr-created).
$SimBackBuffer = @(2560, 1400)      # the simulator's eyes are 1280x1400, side by side
function Set-SimDisplaySettings {
    $f = "$GameDir\DisplaySettings.ini"
    if (-not (Test-Path $f)) { return }
    if (-not (Test-Path "$f.psyvr-backup") -and -not (Test-Path "$f.psyvr-created")) { Copy-Item $f "$f.psyvr-backup" }
    $text = Get-Content $f -Raw
    $text = $text -replace '(?m)^ScreenWidth=[^\r\n]*', "ScreenWidth=$($SimBackBuffer[0])"
    $text = $text -replace '(?m)^ScreenHeight=[^\r\n]*', "ScreenHeight=$($SimBackBuffer[1])"
    [IO.File]::WriteAllText($f, $text, [Text.Encoding]::ASCII)
}

# Short VR check: start (see Start-TestGame), wait for the first VR frame (and
# with -Save / -Continue for that save's level), then -Wait more seconds;
# reports whether the game is alive plus the key log lines. Stops it unless -Keep.
function Quick-Vr([string[]]$Ini = @(), [int]$Wait = 10, [switch]$Keep, [switch]$Continue, [string]$Save = '',
                  [switch]$Force = $true) {
    if ($Continue -and -not $Save) { $Save = 'gpc' }
    Start-TestGame -Ini $Ini -Save $Save -Force:$Force
    if ($null -eq (Wait-Log $PsyLog 'first frame accepted' 300)) { 'no VR frame (see psyvr.log)' }
    elseif ($Save -and $null -eq (Wait-Status (Get-SaveLevel $Save) 300)) { "never reached $(Get-SaveLevel $Save -Long)" }
    elseif ($Save) { "in $(Get-SaveLevel $Save -Long) (save '$Save')" }
    for ($t = 0; $t -lt $Wait; $t += 2) {
        Start-Sleep 2
        if (-not (Get-Process -Name $GameExe -ErrorAction SilentlyContinue)) { break }
    }
    $p = Get-Process -Name $GameExe -ErrorAction SilentlyContinue
    "alive=$([bool]$p) responding=$(if ($p) { $p.Responding })"
    Read-LogLines $PsyLog | Select-String 'HANG|first frame accepted|stereo: game projection|stereo: draws|failed|FAILED|skipped' | Select-Object -Last 8 | ForEach-Object Line
    if (-not $Keep) { Stop-Psy }
}

# --- eye comparison ----------------------------------------------------------

if (-not ('PsyDrive.Eyes' -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace PsyDrive {
public static class Eyes {
    // BGR bytes of the picture scaled to `width` pixels wide.
    static byte[] Load(string path, int width, out int w, out int h, out int srcW) {
        using (var src = new Bitmap(path)) {
            srcW = src.Width;
            w = width; h = Math.Max(1, (int)Math.Round((double)src.Height * width / src.Width));
            using (var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
                using (var g = Graphics.FromImage(bmp)) {
                    g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBilinear;
                    g.DrawImage(src, 0, 0, w, h);
                }
                var d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
                var raw = new byte[d.Stride * h];
                Marshal.Copy(d.Scan0, raw, 0, raw.Length);
                bmp.UnlockBits(d);
                var bgr = new byte[w * h * 3];
                for (int y = 0; y < h; y++) Buffer.BlockCopy(raw, y * d.Stride, bgr, y * w * 3, w * 3);
                return bgr;
            }
        }
    }
    static double[] Grey(byte[] bgr, int n) {
        var g = new double[n];
        for (int i = 0; i < n; i++) g[i] = 0.114 * bgr[3 * i] + 0.587 * bgr[3 * i + 1] + 0.299 * bgr[3 * i + 2];
        return g;
    }
    // [0] mean abs RGB difference, [1] [2] mean grey of L and R, [3] source width,
    // then per band (bands equal horizontal strips, top first): best shift s
    // (R(x+s) ~ L(x), in scaled pixels, ties to the smaller |s|), its cost, the
    // cost at s = 0 (mean abs grey difference).
    public static double[] Compare(string left, string right, int width, int bands, int maxShift) {
        int w, h, sw, w2, h2, sw2;
        var L = Load(left, width, out w, out h, out sw);
        var R = Load(right, width, out w2, out h2, out sw2);
        if (w != w2 || h != h2) throw new ArgumentException("eye pictures differ in size");
        int n = w * h;
        double diff = 0;
        for (int i = 0; i < n * 3; i++) diff += Math.Abs(L[i] - R[i]);
        var gl = Grey(L, n); var gr = Grey(R, n);
        double ll = 0, lr = 0;
        for (int i = 0; i < n; i++) { ll += gl[i]; lr += gr[i]; }
        var res = new double[4 + 3 * bands];
        res[0] = diff / (n * 3); res[1] = ll / n; res[2] = lr / n; res[3] = sw;
        for (int b = 0; b < bands; b++) {
            int y0 = h * b / bands, y1 = h * (b + 1) / bands;
            double best = double.MaxValue, c0 = 0; int bestS = 0;
            for (int k = 0; k <= 2 * maxShift; k++) {
                int s = (k % 2 == 1) ? -(k + 1) / 2 : k / 2;     // 0, -1, 1, -2, 2 ...
                double c = 0; int m = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = maxShift; x < w - maxShift; x++) { c += Math.Abs(gr[y * w + x + s] - gl[y * w + x]); m++; }
                c /= Math.Max(1, m);
                if (s == 0) c0 = c;
                if (c < best) { best = c; bestS = s; }
            }
            res[4 + 3 * b] = bestS; res[5 + 3 * b] = best; res[6 + 3 * b] = c0;
        }
        return res;
    }
}
}
'@
}

# Compares a left and a right eye picture (e.g. from Psy-Shot). Returns
# MeanDiff (mean abs RGB difference, 0-255: ~0 = the eyes got the same
# picture), LumL / LumR (mean grey: ~0 = black), and per horizontal band (top
# first) the best x shift with R(x+s) ~ L(x), in pixels of the eye picture.
# With parallel eyes and a symmetric view, distant things have shift ~0 and
# nearer ones a negative shift; a positive shift means crossed eyes.
function Compare-Eyes([string]$Left, [string]$Right, [int]$Bands = 5, [int]$Width = 480, [int]$MaxShift = 40) {
    $r = [PsyDrive.Eyes]::Compare($Left, $Right, $Width, $Bands, $MaxShift)
    $scale = $r[3] / $Width
    $bandList = for ($b = 0; $b -lt $Bands; $b++) {
        [pscustomobject]@{
            Rows = '{0}-{1}%' -f [int](100 * $b / $Bands), [int](100 * ($b + 1) / $Bands)
            Shift = [Math]::Round($r[4 + 3 * $b] * $scale, 1)
            Cost = [Math]::Round($r[5 + 3 * $b], 2)
            Cost0 = [Math]::Round($r[6 + 3 * $b], 2)
        }
    }
    [pscustomobject]@{
        MeanDiff = [Math]::Round($r[0], 2); LumL = [Math]::Round($r[1], 1); LumR = [Math]::Round($r[2], 1)
        PxPerStep = [Math]::Round($scale, 2); Bands = @($bandList)
    }
}

# The OpenXR simulator's frame rate, read from its preview window's title.
function Get-SimFps {
    $pids = @(Get-Process -Name $GameExe -ErrorAction SilentlyContinue | ForEach-Object Id)
    $script:simTitle = $null
    $cb = [PsyDrive.P+EnumProc]{ param($h, $l)
        $p = 0; [PsyDrive.P]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
        if ($pids -contains $p) {
            $t = New-Object Text.StringBuilder 512; [PsyDrive.P]::GetWindowTextW($h, $t, 512) | Out-Null
            if ($t.ToString() -like 'OpenXR Simulator*') { $script:simTitle = $t.ToString() }
        }
        $true }
    [PsyDrive.P]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($script:simTitle -match 'XR ([\d.]+) FPS avg') { return [double]$matches[1] }
    return $null
}

# --- scripted head poses and sweeps -------------------------------------------

# Puts the head where a test wants it (the pipe's `head` command), on top of
# the tracked head: metres in the recentred room (+x right, +y up, +z back),
# degrees (yaw + = left, pitch + = up, roll + = left ear down). -Off clears it.
function Psy-Head([double]$X = 0, [double]$Y = 0, [double]$Z = 0, [double]$Yaw = 0, [double]$Pitch = 0,
                  [double]$Roll = 0, [switch]$Off) {
    if ($Off) { $cmd = 'head off' }
    else {
        $inv = [Globalization.CultureInfo]::InvariantCulture
        $cmd = 'head ' + (@($X, $Y, $Z, $Yaw, $Pitch, $Roll) | ForEach-Object { $_.ToString('0.####', $inv) }) -join ' '
    }
    $r = @(Psy-Cmd $cmd)
    if ($r[0] -notlike 'ok*') { throw "Psy-Head: $($r -join ' | ')" }
    $r[0]
}

# The pose sets Psy-Sweep knows, as 'label=x,y,z[,yaw,pitch,roll]'. The first
# pose is the reference the others are compared with.
$SweepSets = @{
    basic = @('centre=0,0,0', 'x-5cm=-0.05,0,0', 'x+5cm=0.05,0,0', 'y-5cm=0,-0.05,0', 'y+5cm=0,0.05,0',
              'z-10cm=0,0,-0.10', 'z+10cm=0,0,0.10', 'z-25cm=0,0,-0.25', 'z+25cm=0,0,0.25',
              'yaw+10=0,0,0,10,0,0', 'yaw-10=0,0,0,-10,0,0', 'pitch+10=0,0,0,0,10,0', 'pitch-10=0,0,0,0,-10,0')
    turn  = @('centre=0,0,0', 'yaw+30=0,0,0,30,0,0', 'yaw-30=0,0,0,-30,0,0', 'yaw+70=0,0,0,70,0,0',
              'yaw-70=0,0,0,-70,0,0', 'pitch+30=0,0,0,0,30,0', 'pitch-30=0,0,0,0,-30,0', 'roll+20=0,0,0,0,0,20')
}

if (-not ('PsyDrive.Sheet' -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
namespace PsyDrive {
public static class Sheet {
    static Bitmap Thumb(string path, int w) {
        using (var src = new Bitmap(path)) {
            int h = (int)Math.Round((double)src.Height * w / src.Width);
            var b = new Bitmap(w, h, PixelFormat.Format24bppRgb);
            using (var g = Graphics.FromImage(b)) {
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBilinear;
                g.DrawImage(src, 0, 0, w, h);
            }
            return b;
        }
    }
    static int Grey(Color c) { return (int)(0.299 * c.R + 0.587 * c.G + 0.114 * c.B); }
    // Red = a's grey, cyan = b's grey: grey where they agree, fringes where b moved.
    static Bitmap Anaglyph(Bitmap a, Bitmap b) {
        var o = new Bitmap(a.Width, a.Height, PixelFormat.Format24bppRgb);
        for (int y = 0; y < a.Height; y++)
            for (int x = 0; x < a.Width; x++) {
                int ga = Grey(a.GetPixel(x, y)), gb = Grey(b.GetPixel(x, y));
                o.SetPixel(x, y, Color.FromArgb(ga, gb, gb));
            }
        return o;
    }
    // |b - a| x4 per channel.
    static Bitmap Diff(Bitmap a, Bitmap b) {
        var o = new Bitmap(a.Width, a.Height, PixelFormat.Format24bppRgb);
        for (int y = 0; y < a.Height; y++)
            for (int x = 0; x < a.Width; x++) {
                Color p = a.GetPixel(x, y), q = b.GetPixel(x, y);
                o.SetPixel(x, y, Color.FromArgb(Math.Min(255, 4 * Math.Abs(p.R - q.R)),
                    Math.Min(255, 4 * Math.Abs(p.G - q.G)), Math.Min(255, 4 * Math.Abs(p.B - q.B))));
            }
        return o;
    }
    // One tile per pose: label, L eye, anaglyph, difference. The first pose is
    // the reference: its tile shows its own L/R pair (stereo) instead.
    public static void Make(string[] labels, string[] lefts, string[] rights, string outPng, int thumbW, int cols, string title) {
        var refL = Thumb(lefts[0], thumbW);
        int th = refL.Height, label = 18, pad = 6, top = 26;
        int tileW = thumbW + pad, tileH = label + 3 * th + 2 * 2 + pad;
        int rows = (labels.Length + cols - 1) / cols;
        using (var sheet = new Bitmap(cols * tileW + pad, top + rows * tileH + pad, PixelFormat.Format24bppRgb))
        using (var g = Graphics.FromImage(sheet))
        using (var font = new Font("Segoe UI", 9f))
        using (var bold = new Font("Segoe UI", 10f, FontStyle.Bold)) {
            g.Clear(Color.FromArgb(24, 24, 24));
            g.DrawString(title + "   rows: L eye | anaglyph (red = reference L, cyan = this pose L; first tile: L vs R) | |difference| x4",
                bold, Brushes.White, pad, 5);
            for (int i = 0; i < labels.Length; i++) {
                int x0 = pad + (i % cols) * tileW, y0 = top + (i / cols) * tileH;
                using (var l = Thumb(lefts[i], thumbW)) {
                    Bitmap other = i == 0 ? Thumb(rights[0], thumbW) : null;
                    using (var ana = i == 0 ? Anaglyph(refL, other) : Anaglyph(refL, l))
                    using (var dif = i == 0 ? Diff(refL, other) : Diff(refL, l)) {
                        g.DrawString(labels[i] + (i == 0 ? "  (reference; L vs R)" : ""), font, Brushes.Gainsboro, x0, y0);
                        g.DrawImage(l, x0, y0 + label);
                        g.DrawImage(ana, x0, y0 + label + th + 2);
                        g.DrawImage(dif, x0, y0 + label + 2 * th + 4);
                    }
                    if (other != null) other.Dispose();
                }
            }
            sheet.Save(outPng, ImageFormat.Png);
        }
        refL.Dispose();
    }
}
}
'@
}

# A contact sheet of a sweep folder (Psy-Sweep's manifest.json).
function New-ContactSheet([string]$SweepDir, [int]$ThumbWidth = 240, [int]$Columns = 5, [string]$OutPng = '') {
    $m = Get-Content (Join-Path $SweepDir 'manifest.json') -Raw | ConvertFrom-Json
    if (-not $OutPng) { $OutPng = Join-Path $SweepDir 'contact.png' }
    $labels = @($m.Poses | ForEach-Object Label)
    $lefts = @($m.Poses | ForEach-Object { Join-Path $SweepDir $_.Left })
    $rights = @($m.Poses | ForEach-Object { Join-Path $SweepDir $_.Right })
    [PsyDrive.Sheet]::Make($labels, $lefts, $rights, $OutPng, $ThumbWidth, $Columns, "sweep $($m.Name)")
    $OutPng
}

# Shoots the eyes at a set of scripted head poses, one after the other (each
# frame entirely at its pose; consecutive frames, so animation shows as small
# differences). -Poses: a $SweepSets name or a list of 'label=x,y,z[,yaw,pitch,roll]'.
# Writes <ShotDir>\sweep_<Name>_<time>\: nn_label_L/R.png, manifest.json (each
# pose, its stereo comparison and its comparison with the first pose: band
# shifts of the L eye, in pixels), contact.png. Clears the head script after.
# Needs a running game with the control pipe (Quick-Vr -Continue -Keep).
function Psy-Sweep([string]$Name = 'sweep', [string[]]$Poses = @('basic'), [int]$SettleMs = 150) {
    if ($Poses.Count -eq 1 -and $SweepSets.ContainsKey($Poses[0])) { $Poses = $SweepSets[$Poses[0]] }
    $dir = Join-Path $ShotDir ("sweep_{0}_{1}" -f $Name, (Get-Date -Format 'yyyyMMdd_HHmmss'))
    New-Item -ItemType Directory -Force $dir | Out-Null
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $list = New-Object System.Collections.ArrayList
    try {
        for ($i = 0; $i -lt $Poses.Count; $i++) {
            $label, $vals = $Poses[$i] -split '=', 2
            $v = @($vals -split ',' | ForEach-Object { [double]::Parse($_, $inv) }) + @(0, 0, 0)
            Psy-Head $v[0] $v[1] $v[2] $v[3] $v[4] $v[5] | Out-Null
            Start-Sleep -Milliseconds $SettleMs
            $tag = '{0:D2}_{1}' -f $i, ($label -replace '[^A-Za-z0-9_-]', '_')
            $shot = Psy-Shot "sweep_$tag" -EyesOnly
            Move-Item $shot[0] (Join-Path $dir "${tag}_L.png") -Force
            Move-Item $shot[1] (Join-Path $dir "${tag}_R.png") -Force
            [void]$list.Add([pscustomobject]@{
                Label = $label; X = $v[0]; Y = $v[1]; Z = $v[2]; Yaw = $v[3]; Pitch = $v[4]; Roll = $v[5]
                Left = "${tag}_L.png"; Right = "${tag}_R.png" })
        }
    }
    finally { try { Psy-Head -Off | Out-Null } catch { } }
    $ref = Join-Path $dir $list[0].Left
    foreach ($p in $list) {
        $st = Compare-Eyes (Join-Path $dir $p.Left) (Join-Path $dir $p.Right)
        $p | Add-Member Stereo ([pscustomobject]@{ MeanDiff = $st.MeanDiff; Shifts = @($st.Bands | ForEach-Object Shift) })
        if ($p -ne $list[0]) {
            $mv = Compare-Eyes $ref (Join-Path $dir $p.Left)
            $p | Add-Member VsReference ([pscustomobject]@{ MeanDiff = $mv.MeanDiff; Shifts = @($mv.Bands | ForEach-Object Shift) })
        }
    }
    [pscustomobject]@{ Name = $Name; Time = (Get-Date -Format s); Poses = @($list) } |
        ConvertTo-Json -Depth 6 | Set-Content (Join-Path $dir 'manifest.json') -Encoding UTF8
    New-ContactSheet $dir | Out-Null
    $list | ForEach-Object {
        [pscustomobject]@{ Pose = $_.Label; StereoShifts = ($_.Stereo.Shifts -join ' ')
                           VsRefDiff = $_.VsReference.MeanDiff; VsRefShifts = ($_.VsReference.Shifts -join ' ') }
    } | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    $dir
}

# --- smoke test ---------------------------------------------------------------

# One unattended pass over the whole VR path on the simulator: install, load
# the base save, check the session, the level, the log and the eyes, and print
# PASS / FAIL per item. Eye pictures and summary.json are kept in
# <ShotDir>\smoke_<time>\. Ends with Clean-Game unless -Keep. Refuses to run
# while the game is running unless -Force. Returns the summary object.
function Run-Smoke([string[]]$Ini = @(), [string]$Save = 'gpc', [int]$Settle = 8, [switch]$Keep, [switch]$Force) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = Join-Path $ShotDir ('smoke_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
    New-Item -ItemType Directory -Force $out | Out-Null
    $checks = New-Object System.Collections.ArrayList
    function Add-Check([string]$Name, [bool]$Ok, [string]$Detail) {
        [void]$checks.Add([pscustomobject]@{ Check = $Name; Result = $(if ($Ok) { 'PASS' } else { 'FAIL' }); Detail = $Detail })
    }
    $eyes = $null; $simFps = $null; $perf = $null
    try {
        Start-TestGame -Ini $Ini -Save $Save -Force:$Force
        $up = Wait-Log $PsyLog 'vr: up' 300
        Add-Check 'OpenXR session up' ($null -ne $up) "$up"
        $first = Wait-Log $PsyLog 'first frame accepted' 120
        Add-Check 'first VR frame' ($null -ne $first) "$first"
        $long = Get-SaveLevel $Save -Long
        $lvl = Wait-Log $AstraLog ("LOADING LEVEL " + [regex]::Escape($long)) 300
        $status = if ($lvl) { Wait-Status (Get-SaveLevel $Save) 120 } else { $null }
        Add-Check "save '$Save' loaded ($long)" ($null -ne $status) ("$($status -join ' ')".Trim())
        if ($status) {
            Start-Sleep $Settle
            $shot = Psy-Shot smoke
            Copy-Item $shot[0], $shot[1], $shot[2] $out -Force
            $l = Join-Path $out (Split-Path $shot[0] -Leaf); $r = Join-Path $out (Split-Path $shot[1] -Leaf)
            $eyes = Compare-Eyes $l $r
            Add-Check 'eyes not black' ($eyes.LumL -gt 8 -and $eyes.LumR -gt 8) ("mean grey L {0} R {1}" -f $eyes.LumL, $eyes.LumR)
            Add-Check 'eyes differ (stereo)' ($eyes.MeanDiff -ge 0.5) ("mean RGB difference {0}" -f $eyes.MeanDiff)
            $shifts = @($eyes.Bands | ForEach-Object Shift)
            $crossed = @($shifts | Where-Object { $_ -gt $eyes.PxPerStep })
            $near = @($shifts | Where-Object { $_ -lt -$eyes.PxPerStep })
            Add-Check 'disparity sign (nearer = negative)' ($crossed.Count -eq 0 -and $near.Count -gt 0) ("band shifts px: " + ($shifts -join ', '))
            $simFps = Get-SimFps
        }
        $p = Get-Process -Name $GameExe -ErrorAction SilentlyContinue
        Add-Check 'game alive and responding' ([bool]$p -and $p.Responding) ''
        $log = Read-LogLines $PsyLog
        $bad = @($log | Select-String 'HANG|FAILED|failed|skipped \(code at' | ForEach-Object Line)
        Add-Check 'no HANG / failed / skipped-patch lines' ($bad.Count -eq 0) (($bad | Select-Object -First 3) -join ' | ')
        $pl = $log | Select-String 'perf: ([\d.]+) fps' | Select-Object -Last 1
        if ($pl) { $perf = [double]$pl.Matches[0].Groups[1].Value }
        Set-Content (Join-Path $out 'psyvr.log') -Encoding UTF8 -Value $log
    }
    catch { Add-Check 'harness' $false $_.Exception.Message }
    finally { if (-not $Keep) { Clean-Game | Out-Null } }

    $failed = @($checks | Where-Object Result -eq 'FAIL').Count
    $summary = [pscustomobject]@{
        Passed = ($failed -eq 0); Failed = $failed; Seconds = [int]$sw.Elapsed.TotalSeconds
        SimFps = $simFps; ModFps = $perf; Folder = $out; Checks = @($checks); Eyes = $eyes
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'summary.json') -Encoding UTF8
    $checks | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    if ($eyes) { $eyes.Bands | Format-Table -AutoSize | Out-String | Write-Host }
    Write-Host ("{0}: {1} failed, {2} s, simulator {3} fps, mod {4} fps -> {5}" -f
        $(if ($failed -eq 0) { 'SMOKE PASS' } else { 'SMOKE FAIL' }), $failed, $summary.Seconds, $simFps, $perf, $out)
    return $summary
}

# --- spots: where Raz and the game camera are, recorded by name --------------

# Reads Raz's position (*(*(0x78BC20)+0x818C)+0x10 -> +0x40) and the active
# camera (*(*(0x78BC20)+0x8C98): position +0x08, quaternion +0x20, cull FOV
# +0x30) from the running game's memory. With -Name, also appends the record
# to $SpotsFile (the spots of future runs).
function Psy-Where([string]$Name = '', [string]$Note = '') {
    if (-not ('PsyMem.R' -as [type])) {
        Add-Type -Namespace PsyMem -Name R -MemberDefinition @'
[DllImport("kernel32")] public static extern System.IntPtr OpenProcess(int a, bool b, int pid);
[DllImport("kernel32")] public static extern bool ReadProcessMemory(System.IntPtr h, System.IntPtr addr, byte[] buf, int n, out int r);
[DllImport("kernel32")] public static extern bool CloseHandle(System.IntPtr h);
'@
    }
    $p = Get-Process -Name $GameExe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { throw 'game not running' }
    $h = [PsyMem.R]::OpenProcess(0x10, $false, $p.Id)
    try {
        $rd = { param($a, $n) $b = New-Object byte[] $n; $r = 0; [void][PsyMem.R]::ReadProcessMemory($h, [IntPtr][long]$a, $b, $n, [ref]$r); ,$b }
        $u32 = { param($a) [BitConverter]::ToUInt32((& $rd $a 4), 0) }
        $f3 = { param($a, $n) $b = & $rd $a (4 * $n); @(0..($n - 1) | ForEach-Object { [math]::Round([BitConverter]::ToSingle($b, 4 * $_), 4) }) }
        $game = & $u32 0x78BC20
        $raz = $null
        $ent = & $u32 ($game + 0x818C)
        $razRows = $null
        if ($ent) { $body = & $u32 ($ent + 0x10); if ($body) { $raz = & $f3 ($body + 0x40) 3; $razRows = & $f3 ($body + 0x10) 12 } }
        $cam = & $u32 ($game + 0x8C98)
        $cpos = & $f3 ($cam + 0x08) 3
        $q = & $f3 ($cam + 0x20) 4
        $fov = (& $f3 ($cam + 0x30) 1)[0]
    }
    finally { [void][PsyMem.R]::CloseHandle($h) }
    # The camera looks down -Z of its rotation (god_seat.h): forward = R * (0,0,-1).
    $x, $y, $z, $w = $q
    $fx = -(2 * ($x * $z + $w * $y)); $fy = -(2 * ($y * $z - $w * $x)); $fz = -(1 - 2 * ($x * $x + $y * $y))
    $yaw = [math]::Round([math]::Atan2($fx, -$fz) * 180 / [math]::PI, 1)
    $pitch = [math]::Round([math]::Atan2($fy, [math]::Sqrt($fx * $fx + $fz * $fz)) * 180 / [math]::PI, 1)
    # Raz's world matrix rows (body + 0x10): right, up, forward (+0x30). His
    # heading in the camera's yaw convention, atan2(x, -z) of his forward.
    $razYaw = $null
    if ($razRows) { $razYaw = [math]::Round([math]::Atan2($razRows[8], -$razRows[10]) * 180 / [math]::PI, 1) }
    $level = ''
    try { $s = @(Psy-Cmd status); if (($s -join ' ') -match 'level (\S+)') { $level = $Matches[1] } } catch {}
    $rec = [pscustomobject]@{ Name = $Name; Note = $Note; Time = (Get-Date -Format s); Level = $level; Raz = $raz
        RazMatrix = $razRows; RazYaw = $razYaw
        CamPos = $cpos; CamQuat = $q; CamYaw = $yaw; CamPitch = $pitch; CullFovLua = $fov }
    if ($Name) {
        $file = $SpotsFile
        # Windows PowerShell's ConvertFrom-Json hands a JSON array back as one
        # object: enumerate it, and write with -InputObject so it stays flat.
        $all = New-Object System.Collections.ArrayList
        if (Test-Path $file) { foreach ($r in (Get-Content $file -Raw | ConvertFrom-Json)) { [void]$all.Add($r) } }
        [void]$all.Add($rec)
        ConvertTo-Json -InputObject $all.ToArray() -Depth 4 | Set-Content $file -Encoding ascii
        # The eye shot, kept beside the spots (Clean-Game removes psyvr_shots).
        try {
            Psy-Shot $Name -EyesOnly | Out-Null
            $dir = $SpotsDir
            New-Item -ItemType Directory -Force $dir | Out-Null
            Copy-Item "$GameDir\psyvr_shots\$($Name)_*.png" $dir -Force
        } catch { Write-Host "spot shot failed: $_" }
    }
    $rec
}

# Puts Raz at a spot from spots.json (or -Pos x,y,z): writes the position row
# of his world matrix (body + 0x40, docs/findings.md 2026-11-15). The area
# there only loads once Raz moves, so a short step forward follows (-NoStep
# leaves it out) and then the position is written again.
function Psy-Teleport([string]$Name = '', [double[]]$Pos = $null, [switch]$NoStep) {
    $rows = $null     # the spot's rotation rows (RazMatrix), written too when recorded
    if (-not $Pos) {
        if (-not $Name) { throw 'Psy-Teleport <spot name> or -Pos x,y,z' }
        $spot = $null
        foreach ($r in (Get-Content $SpotsFile -Raw | ConvertFrom-Json)) { if ($r.Name -eq $Name) { $spot = $r } }
        if (-not $spot) { throw "no spot '$Name' in spots.json" }
        $Pos = @($spot.Raz)
        if ($spot.RazMatrix -and @($spot.RazMatrix).Count -eq 12) { $rows = @($spot.RazMatrix) }
        $level = ''
        try { $s = @(Psy-Cmd status); if (($s -join ' ') -match 'level (\S+)') { $level = $Matches[1] } } catch {}
        if ($spot.Level -and $level -and $spot.Level -ne $level) { throw "spot '$Name' is in $($spot.Level), the game is in $level" }
    }
    if ($Pos.Count -ne 3) { throw '-Pos takes x,y,z' }
    if (-not ('PsyMem.RW' -as [type])) {
        Add-Type -Namespace PsyMem -Name RW -MemberDefinition @'
[DllImport("kernel32")] public static extern System.IntPtr OpenProcess(int a, bool b, int pid);
[DllImport("kernel32")] public static extern bool ReadProcessMemory(System.IntPtr h, System.IntPtr addr, byte[] buf, int n, out int r);
[DllImport("kernel32")] public static extern bool WriteProcessMemory(System.IntPtr h, System.IntPtr addr, byte[] buf, int n, out int r);
[DllImport("kernel32")] public static extern bool CloseHandle(System.IntPtr h);
'@
    }
    $p = Get-Process -Name $GameExe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { throw 'game not running' }
    $write = {
        $h = [PsyMem.RW]::OpenProcess(0x38, $false, $p.Id)
        try {
            $u32 = { param($a) $b = New-Object byte[] 4; $r = 0; [void][PsyMem.RW]::ReadProcessMemory($h, [IntPtr][long]$a, $b, 4, [ref]$r); [BitConverter]::ToUInt32($b, 0) }
            $ent = & $u32 ((& $u32 0x78BC20) + 0x818C)
            if (-not $ent) { throw 'no player entity' }
            $body = & $u32 ($ent + 0x10)
            if (-not $body) { throw 'no player body' }
            $b = New-Object byte[] 12
            for ($i = 0; $i -lt 3; $i++) { [BitConverter]::GetBytes([float]$Pos[$i]).CopyTo($b, 4 * $i) }
            $r = 0
            if (-not [PsyMem.RW]::WriteProcessMemory($h, [IntPtr][long]($body + 0x40), $b, 12, [ref]$r)) { throw 'write failed' }
            if ($rows) {
                $m = New-Object byte[] 48
                for ($i = 0; $i -lt 12; $i++) { [BitConverter]::GetBytes([float]$rows[$i]).CopyTo($m, 4 * $i) }
                if (-not [PsyMem.RW]::WriteProcessMemory($h, [IntPtr][long]($body + 0x10), $m, 48, [ref]$r)) { throw 'rotation write failed' }
            }
        }
        finally { [void][PsyMem.RW]::CloseHandle($h) }
    }
    & $write
    if ($NoStep) { Start-Sleep -Milliseconds 300 }
    else {
        Pad-Script @('0.5 ly=1', '0.5') -Wait
        & $write
    }
    Psy-Where
}

# From a loaded `circus` save: to the tent with the enemy for targeting tests
# (it barely moves and does not attack). Teleporting straight in skips the
# door's trigger and no enemy spawns, so: the door, a walk through it (the
# interior is at Y ~ -23392), then the recorded spot and Raz's facing.
function Psy-GotoEnemy {
    Psy-Teleport aiming_door | Out-Null
    $w = Psy-Walk -Seconds 2 -Spot aiming_door
    if (-not $w.Raz -or $w.Raz[1] -gt -20000) { throw "the walk did not go through the door (Raz at $($w.Raz -join ', '))" }
    Start-Sleep 1
    Psy-Teleport aiming_tent_enemy -NoStep
}

# Walks Raz for -Seconds towards a world heading (-Yaw, degrees, as Psy-Where's
# CamYaw: atan2(x, -z)), or the spot's recorded camera heading (-Spot). The
# stick is camera-relative, so it is turned by the camera's heading now; the
# camera swings behind Raz as he walks, so keep the walks short.
function Psy-Walk([double]$Seconds = 1, [double]$Yaw = [double]::NaN, [string]$Spot = '') {
    if ($Spot) {
        foreach ($r in (Get-Content $SpotsFile -Raw | ConvertFrom-Json)) { if ($r.Name -eq $Spot) { $Yaw = $r.CamYaw } }
        if ([double]::IsNaN($Yaw)) { throw "no spot '$Spot' in spots.json" }
    }
    if ([double]::IsNaN($Yaw)) { throw 'Psy-Walk -Yaw deg or -Spot name' }
    $d = ($Yaw - (Psy-Where).CamYaw) * [math]::PI / 180
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $lx = [math]::Round([math]::Sin($d), 3).ToString($inv)
    $ly = [math]::Round([math]::Cos($d), 3).ToString($inv)
    Pad-Script @("$($Seconds.ToString($inv)) lx=$lx ly=$ly", '0.3') -Wait
    Psy-Where
}
