# Brings the game to the front and presses keys (virtual-key names, e.g. F5, W, SPACE). Lab tool.
#   key.ps1 F5            press and release
#   key.ps1 W -HoldMs 2000  hold for 2 s
param([Parameter(Mandatory, Position = 0)][string[]]$Keys, [int]$HoldMs = 80, [string]$ProcessName = "GTA5")
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace Lab -Name Keys -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
'@
$p = Get-Process $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
# Windows only lets a background process take the foreground right after input; a tap of Alt counts.
for ($i = 0; $i -lt 5 -and [Lab.Keys]::GetForegroundWindow() -ne $p.MainWindowHandle; $i++) {
    [Lab.Keys]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [Lab.Keys]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [void][Lab.Keys]::SetForegroundWindow($p.MainWindowHandle)
    Start-Sleep -Milliseconds 250
}
if ([Lab.Keys]::GetForegroundWindow() -ne $p.MainWindowHandle) { throw "the game is not in the foreground; no key sent" }
foreach ($k in $Keys) {
    $vk = [byte][System.Windows.Forms.Keys]::Parse([System.Windows.Forms.Keys], $k)
    $scan = [byte][Lab.Keys]::MapVirtualKeyW($vk, 0)
    # Arrows and the navigation block are extended keys; without the flag they arrive as the numpad keys.
    $ext = if ($vk -in 0x21..0x28 + 0x2D, 0x2E) { 1 } else { 0 }
    [Lab.Keys]::keybd_event($vk, $scan, $ext, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [Lab.Keys]::keybd_event($vk, $scan, 2 -bor $ext, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 100
}
"sent: $($Keys -join ' ')"
