param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$OutPng,
    [int]$WaitMs = 15000
)

Add-Type -AssemblyName System.Drawing

$src = @"
using System;
using System.Runtime.InteropServices;
public class W32Cap {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
Add-Type -TypeDefinition $src

$workDir = Split-Path -Parent $Exe
$proc = Start-Process -FilePath $Exe -WorkingDirectory $workDir -PassThru

$deadline = (Get-Date).AddMilliseconds($WaitMs)
while (-not $proc.HasExited -and $proc.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    $proc.Refresh()
}

if ($proc.HasExited) {
    Write-Output "PROCESS_EXITED code=$($proc.ExitCode)"
    exit 2
}

$hwnd = $proc.MainWindowHandle
if ($hwnd -eq 0) {
    Write-Output "NO_WINDOW_HANDLE"
    $proc.Kill()
    exit 3
}

Start-Sleep -Milliseconds 6000

# Restore the window if it came up minimised; otherwise GetWindowRect returns the
# tiny taskbar rect and the capture is useless.
[void][W32Cap]::ShowWindow($hwnd, 9)
[void][W32Cap]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 2000

$rect = New-Object W32Cap+RECT
[void][W32Cap]::GetWindowRect($hwnd, [ref]$rect)
$winW = $rect.Right - $rect.Left
$winH = $rect.Bottom - $rect.Top
Write-Output "WINDOW $winW x $winH"

if ($winW -lt 200 -or $winH -lt 200) {
    Write-Output "BAD_RECT_TOO_SMALL"
    $proc.Kill()
    exit 4
}

$bmp = New-Object System.Drawing.Bitmap $winW, $winH
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $gfx.GetHdc()
# PW_RENDERFULLCONTENT (0x2) is required to capture DirectX-rendered content
$ok = [W32Cap]::PrintWindow($hwnd, $hdc, 2)
$gfx.ReleaseHdc($hdc)
Write-Output "PRINTWINDOW_RESULT $ok"

$dir = Split-Path -Parent $OutPng
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
$bmp.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose(); $bmp.Dispose()
Write-Output "CAPTURED $OutPng"

if (-not $proc.HasExited) { $proc.Kill() }
Write-Output "DONE"
