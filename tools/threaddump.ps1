# Prints where every thread of a process is: instruction pointer plus code addresses found on its stack, as
# module+offset. For diagnosing hangs from outside the game. Lab tool.
param([string]$ProcessName = "GTA5", [int]$Frames = 14)
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class Dumper {
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr p, IntPtr a, byte[] b, IntPtr n, out IntPtr r);
    public static List<string> Dump(int pid, int frames) {
        var proc = Process.GetProcessById(pid);
        var mods = new List<Tuple<long, long, string>>();
        foreach (ProcessModule m in proc.Modules) mods.Add(Tuple.Create((long)m.BaseAddress, (long)m.BaseAddress + m.ModuleMemorySize, m.ModuleName));
        Func<long, string> name = a => { foreach (var m in mods) if (a >= m.Item1 && a < m.Item2) return m.Item3 + "+" + (a - m.Item1).ToString("X"); return null; };
        IntPtr ph = OpenProcess(0x10, false, pid);
        var lines = new List<string>();
        IntPtr ctx = Marshal.AllocHGlobal(1232 + 16);
        IntPtr c = new IntPtr((ctx.ToInt64() + 15) & ~15L);
        foreach (ProcessThread t in proc.Threads) {
            IntPtr h = OpenThread(0x0002 | 0x0008, false, (uint)t.Id);
            if (h == IntPtr.Zero) continue;
            var trail = new List<string>();
            if (SuspendThread(h) != 0xFFFFFFFF) {
                for (int i = 0; i < 1232; i++) Marshal.WriteByte(c, i, 0);
                Marshal.WriteInt32(c, 0x30, 0x100003);   // CONTEXT_CONTROL | CONTEXT_INTEGER
                if (GetThreadContext(h, c)) {
                    long rip = Marshal.ReadInt64(c, 0xF8), rsp = Marshal.ReadInt64(c, 0x98);
                    trail.Add(name(rip) ?? rip.ToString("X"));
                    var buf = new byte[65536]; IntPtr got;
                    if (ReadProcessMemory(ph, new IntPtr(rsp), buf, new IntPtr(buf.Length), out got) || got.ToInt64() > 0)
                        for (int off = 0; off + 8 <= got.ToInt64() && trail.Count < frames; off += 8) {
                            long v = BitConverter.ToInt64(buf, off);
                            string n = name(v);
                            if (n != null && !n.StartsWith("GTA5.exe+16") ) trail.Add(n);
                            else if (n != null) trail.Add(n);
                        }
                }
                ResumeThread(h);
            }
            CloseHandle(h);
            lines.Add(t.Id.ToString().PadLeft(6) + " " + t.ThreadState + "/" + (t.ThreadState == ThreadState.Wait ? t.WaitReason.ToString() : "") + "  " + string.Join(" < ", trail));
        }
        CloseHandle(ph);
        return lines;
    }
}
'@
$p = Get-Process $ProcessName -ErrorAction Stop | Select-Object -First 1
[Dumper]::Dump($p.Id, $Frames)
