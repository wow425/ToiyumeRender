
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W2 {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
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
$exe = "D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender\out\build\Debug\Demos\DeferredApp\DeferredApp.exe"
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
Start-Sleep -Seconds 5
$hwnd = [W2]::FindByPid([uint32]$p.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "no window"; $p.Kill(); exit 1 }

# 先缩到 900x600 等几秒，让它输出截图
[void][W2]::SetWindowPos($hwnd, [IntPtr]::Zero, 100, 100, 900, 600, 0x0004)
Start-Sleep -Seconds 6
[void][W2]::SetWindowPos($hwnd, [IntPtr]::Zero, 100, 100, 1400, 500, 0x0004)
Start-Sleep -Seconds 6
[void][W2]::SetWindowPos($hwnd, [IntPtr]::Zero, 100, 100, 700, 900, 0x0004)
Start-Sleep -Seconds 6
Write-Output ("alive=" + (-not $p.HasExited))
$p.Kill()
