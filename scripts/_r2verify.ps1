
$ErrorActionPreference = 'Stop'
$root = 'D:\CS-Self-Study\Computer_Graphics\DX12\TooiyumeRender'
$exe  = Join-Path $root 'out\build\Debug\Demos\DeferredApp\DeferredApp.exe'
$wd   = Split-Path $exe
$out  = Join-Path $root 'out\r2_verify'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null
$log = Join-Path $wd 'tooiyume_log.txt'
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath $exe -WorkingDirectory $wd -ArgumentList @('-ResizeTest','-Screenshot',$out) -PassThru
Start-Sleep -Seconds 30
$alive = -not $p.HasExited
if ($alive) { $p.Kill(); $p.WaitForExit() }
Write-Output ("alive_before_kill=" + $alive + " exitcode=" + $p.ExitCode)
Write-Output ("png_count=" + (Get-ChildItem $out -Filter *.png -ErrorAction SilentlyContinue).Count)
Get-ChildItem $out -Filter *.png | ForEach-Object { Write-Output ("  " + $_.Name) }
