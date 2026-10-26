param([int]$Iterations = 100)

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Win {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [Out] StringBuilder s, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    public static IntPtr FindByPid(uint target) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target && IsWindowVisible(h)) {
                var sb = new StringBuilder(256);
                GetWindowTextW(h, sb, 256);
                if (sb.Length > 0) { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

$exe = "D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender\out\build\Debug\Demos\DeferredApp\DeferredApp.exe"
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
Write-Output "started pid=$($p.Id)"
Start-Sleep -Seconds 5

$hwnd = [Win]::FindByPid([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Output "FAIL: no window (exited=$($p.HasExited))"
    if (-not $p.HasExited) { $p.Kill() }
    exit 1
}
Write-Output "hwnd=$hwnd"

$sizes = @(@(1280,720),@(1920,1080),@(800,600),@(1000,700),@(1600,400),@(400,900),@(640,480),@(1366,768),@(1920,1200),@(500,500))
$fail = 0
for ($i = 0; $i -lt $Iterations; $i++) {
    if ($p.HasExited) { Write-Output "FAIL: exited at iter $i code=$($p.ExitCode)"; $fail=1; break }
    $s = $sizes[$i % $sizes.Count]
    [void][Win]::SetWindowPos($hwnd, [IntPtr]::Zero, 100, 100, $s[0], $s[1], 0x0004)
    Start-Sleep -Milliseconds 70
    if ($i -eq 25 -or $i -eq 60) {
        [void][Win]::ShowWindow($hwnd, 6); Start-Sleep -Milliseconds 800
        [void][Win]::ShowWindow($hwnd, 9); Start-Sleep -Milliseconds 800
        Write-Output "  [min/restore @ $i]"
    }
}
Start-Sleep -Seconds 2
if ($p.HasExited) { Write-Output "FAIL: exited after loop code=$($p.ExitCode)"; $fail=1 }
else {
    $memMB = [math]::Round($p.WorkingSet64/1MB,1)
    Write-Output ("survived. mem=" + $memMB + "MB")
    $p.Kill()
}
if ($fail) { exit 1 }
Write-Output "PASS"
