# Runs a lab sequence in the real game: (re)start GTA V through the launcher, wait for story mode, then do steps.
#   labrun.ps1 -Steps "F5","hold:W:4000","F9","wait:3000","shot:after_load" [-NoRestart] [-Name run1]
# Steps: a key name (pressed once), hold:KEY:ms, wait:ms, shot:name (lab\<Name>_<name>.png), log (prints new log lines).
param([string[]]$Steps = @(), [switch]$NoRestart, [string]$Name = "run", [string]$Bin = "build")
$root = Split-Path $PSScriptRoot -Parent
$log = "$root\$Bin\savestate.log"
$tools = $PSScriptRoot

function Wait-Story {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 180) {
        if (((Get-Date) - $t0).TotalSeconds -gt 20 -and -not (Get-Process GTA5 -ErrorAction SilentlyContinue)) { throw "GTA5 exited while starting" }
        $regions = @(Select-String -Path $log -Pattern 'game region .*: 256 MB' -ErrorAction SilentlyContinue)
        # Two 256 MB regions at start-up, a third one when story mode loads.
        if ($regions.Count -ge 3) { Start-Sleep 15; return }
        Start-Sleep 1
    }
    throw "story mode did not load within 3 minutes"
}

if (-not $NoRestart) {
    Get-Process GTA5, PlayGTAV -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    Start-Process -FilePath "$root\$Bin\GTA5-Savestates.exe"
    Start-Sleep 5
    Wait-Story
    "story mode loaded"
}
$seen = (Get-Content $log).Count
foreach ($s in $Steps) {
    if (-not (Get-Process GTA5 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })) { "GAME IS GONE before step '$s'"; break }
    $parts = $s.Split(':')
    switch ($parts[0]) {
        'hold' { & "$tools\key.ps1" $parts[1] -HoldMs ([int]$parts[2]) | Out-Null }
        'wait' { Start-Sleep -Milliseconds ([int]$parts[1]) }
        'shot' { & "$tools\shot.ps1" -Out "$root\lab\${Name}_$($parts[1]).png" -Width 800 -Front | Out-Null }
        'log' { $all = Get-Content $log; $all | Select-Object -Skip $seen; $seen = $all.Count }
        default {
            & "$tools\key.ps1" $s | Out-Null
            if ($s -in 'F5', 'F9') {
                # Wait for the savestate code to report.
                $t0 = Get-Date
                do { Start-Sleep -Milliseconds 300; $new = Get-Content $log | Select-Object -Skip $seen } until (($new -match 'saved \(|loaded( from an earlier game session)? \(|Cannot (save|load)') -or ((Get-Date) - $t0).TotalSeconds -gt 60)
            }
        }
    }
}
$all = Get-Content $log; $all | Select-Object -Skip $seen
$g = Get-Process GTA5 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
if ($g) { "game still running (pid $($g.Id))" } else { "GAME NOT RUNNING" }
