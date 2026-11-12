# Screenshot of a window's client area, scaled down. Lab tool.
#   shot.ps1 -Out shot.png [-ProcessName GTA5] [-Width 1280] [-Front]
param([Parameter(Mandatory)][string]$Out, [string]$ProcessName = "GTA5", [int]$Width = 1280, [switch]$Front)
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Lab -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern bool GetClientRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(System.IntPtr h, ref POINT p);
public struct RECT { public int L, T, R, B; }
public struct POINT { public int X, Y; }
'@
[void][Lab.Win]::SetProcessDPIAware()
$p = Get-Process $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
if ($Front) {
    for ($i = 0; $i -lt 5 -and [Lab.Win]::GetForegroundWindow() -ne $h; $i++) {
        [Lab.Win]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [Lab.Win]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
        [void][Lab.Win]::SetForegroundWindow($h)
        Start-Sleep -Milliseconds 300
    }
}
$r = New-Object Lab.Win+RECT; [void][Lab.Win]::GetClientRect($h, [ref]$r)
$pt = New-Object Lab.Win+POINT; [void][Lab.Win]::ClientToScreen($h, [ref]$pt)
$w = $r.R - $r.L; $hgt = $r.B - $r.T
$bmp = New-Object Drawing.Bitmap $w, $hgt
$g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size); $g.Dispose()
$sw = [Math]::Min($Width, $w); $sh = [int]($hgt * $sw / $w)
$small = New-Object Drawing.Bitmap $bmp, $sw, $sh
$small.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose(); $small.Dispose()
"saved $Out ({0}x{1} from {2}x{3} at {4},{5})" -f $sw, $sh, $w, $hgt, $pt.X, $pt.Y
