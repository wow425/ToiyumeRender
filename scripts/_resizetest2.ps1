param([int]$Iterations = 100)
$ErrorActionPreference = 'Stop'
$root = 'D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender'
$exe  = Join-Path $root 'out\build\Debug\Demos\DeferredApp\DeferredApp.exe'
$wd   = Split-Path $exe
$progress = Join-Path $root 'out\stress_progress.txt'
"start $(Get-Date -Format o)" | Set-Content $progress
$log = Join-Path $wd 'tooiyume_log.txt'
if (Test-Path $log) { Remove-Item $log -Force }

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W3 {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [Out] StringBuilder s, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    public static IntPtr FindByPid(uint t) {
        IntPtr f = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == t && IsWindowVisible(h)) {
                var sb = new StringBuilder(256); GetWindowTextW(h, sb, 256);
                if (sb.Length > 0) { f = h; return false; }
            } return true; }, IntPtr.Zero);
        return f;
    }
}
"@

$p = Start-Process -FilePath $exe -WorkingDirectory $wd -PassThru
"launched pid=$($p.Id)" | Add-Content $progress
Start-Sleep -Seconds 6
if ($p.HasExited) { "FAIL exited early code=$($p.ExitCode)" | Add-Content $progress; Get-Content $progress; exit 1 }

$hwnd = [W3]::FindByPid([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { "FAIL no window" | Add-Content $progress; $p.Kill(); Get-Content $progress; exit 1 }
"hwnd=$hwnd" | Add-Content $progress

$sizes = @(@(1280,720),@(1920,1080),@(800,600),@(1000,700),@(1600,400),@(400,900),@(640,480),@(1366,768),@(1920,1200),@(500,500))
$fails = 0
for ($i = 0; $i -lt $Iterations; $i++) {
    if ($p.HasExited) { "FAIL exited iter=$i code=$($p.ExitCode)" | Add-Content $progress; $fails=1; break }
    $s = $sizes[$i % $sizes.Count]
    [void][W3]::SetWindowPos($hwnd, [IntPtr]::Zero, 60, 60, $s[0], $s[1], 0x0004)
    Start-Sleep -Milliseconds 90
    if (($i % 10) -eq 0) { "iter=$i size=$($s[0])x$($s[1]) mem=$([math]::Round($p.WorkingSet64/1MB,1))MB" | Add-Content $progress }
    if ($i -eq 30 -or $i -eq 70) {
        [void][W3]::ShowWindow($hwnd, 6); Start-Sleep -Milliseconds 700
        [void][W3]::ShowWindow($hwnd, 9); Start-Sleep -Milliseconds 900
        "  min/restore @ $i" | Add-Content $progress
    }
}
Start-Sleep -Seconds 2
if ($p.HasExited) { "FAIL exited after loop code=$($p.ExitCode)" | Add-Content $progress; $fails=1 }
else {
    "survived mem=$([math]::Round($p.WorkingSet64/1MB,1))MB" | Add-Content $progress
    $p.Kill(); $p.WaitForExit()
}
if ($fails) { "RESULT=FAIL" | Add-Content $progress; Get-Content $progress; exit 1 }
"RESULT=PASS" | Add-Content $progress
Get-Content $progress
