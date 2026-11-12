# Loads a DLL into a running process with CreateRemoteThread(LoadLibraryW). Lab tool.
#   inject.ps1 -ProcessName GTA5 -Dll "E:\...\recon.asi"
param([string]$ProcessName = "GTA5", [Parameter(Mandatory)][string]$Dll)
Add-Type -Namespace Lab -Name Inject -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern System.IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll", SetLastError=true)] public static extern System.IntPtr VirtualAllocEx(System.IntPtr p, System.IntPtr a, System.UIntPtr n, uint t, uint prot);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool WriteProcessMemory(System.IntPtr p, System.IntPtr a, byte[] b, System.UIntPtr n, out System.UIntPtr w);
[DllImport("kernel32.dll", SetLastError=true)] public static extern System.IntPtr CreateRemoteThread(System.IntPtr p, System.IntPtr sa, System.UIntPtr stack, System.IntPtr fn, System.IntPtr arg, uint flags, System.IntPtr tid);
[DllImport("kernel32.dll", SetLastError=true)] public static extern uint WaitForSingleObject(System.IntPtr h, uint ms);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetExitCodeThread(System.IntPtr h, out uint code);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)] public static extern System.IntPtr GetProcAddress(System.IntPtr m, string n);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern System.IntPtr GetModuleHandleW(string n);
'@
$p = Get-Process $ProcessName -ErrorAction Stop | Select-Object -First 1
$h = [Lab.Inject]::OpenProcess(0x1F0FFF, $false, $p.Id)
if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed: " + [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
$bytes = [Text.Encoding]::Unicode.GetBytes($Dll + [char]0)
$mem = [Lab.Inject]::VirtualAllocEx($h, [IntPtr]::Zero, ([UIntPtr]::new([uint64]4096)), 0x3000, 4)
$w = [UIntPtr]::Zero
[void][Lab.Inject]::WriteProcessMemory($h, $mem, $bytes, ([UIntPtr]::new([uint64]$bytes.Length)), [ref]$w)
$ll = [Lab.Inject]::GetProcAddress([Lab.Inject]::GetModuleHandleW("kernel32.dll"), "LoadLibraryW")
$t = [Lab.Inject]::CreateRemoteThread($h, [IntPtr]::Zero, [UIntPtr]::Zero, $ll, $mem, 0, [IntPtr]::Zero)
if ($t -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed: " + [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
[void][Lab.Inject]::WaitForSingleObject($t, 10000)
$code = 0
[void][Lab.Inject]::GetExitCodeThread($t, [ref]$code)
"LoadLibraryW in pid {0} returned (low 32 bits) 0x{1:X8}" -f $p.Id, $code

