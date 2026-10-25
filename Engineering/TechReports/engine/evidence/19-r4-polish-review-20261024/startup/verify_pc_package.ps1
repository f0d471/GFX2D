param([Parameter(Mandatory=$true)][string]$Package,[string]$Label='pc',[switch]$Fullscreen,[switch]$Keep)
$ErrorActionPreference='Stop'
$taskRepo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$taskBundle=Join-Path $taskRepo ('Main/bit/'+$Package)
$taskExe=[IO.Path]::GetFullPath((Join-Path $taskBundle 'pc/gfx2dGame.exe'))
$taskExisting=@(Get-Process -Name gfx2dGame -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $taskExe} | ForEach-Object {$_.Id})
$taskLog=Join-Path $PSScriptRoot ('launch-'+$Label+'.log')
$taskErr=Join-Path $PSScriptRoot ('launch-'+$Label+'.stderr.log')
$taskArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $taskBundle 'Run-PC.ps1'))
if($Fullscreen){$taskArgs+='-Fullscreen'}
$taskLauncher=Start-Process powershell.exe -ArgumentList $taskArgs -WindowStyle Hidden -RedirectStandardOutput $taskLog -RedirectStandardError $taskErr -PassThru
$taskGame=$null
for($taskTry=0;$taskTry -lt 80;$taskTry++){
    $taskGame=Get-Process -Name gfx2dGame -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $taskExe -and $_.Id -notin $taskExisting} | Select-Object -First 1
    if($taskGame -and $taskGame.MainWindowHandle -ne 0){break}
    Start-Sleep -Milliseconds 100
}
if(-not $taskGame -or $taskGame.MainWindowHandle -eq 0){throw ('No game window; inspect '+$taskLog)}
Start-Sleep -Seconds 2
$taskGame.Refresh();if($taskGame.HasExited){throw 'Game exited during startup'}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PackageGameWindow {
 [StructLayout(LayoutKind.Sequential)] public struct RECT {public int L,T,R,B;}
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int w,int z,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
'@
[PackageGameWindow]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))|Out-Null
[PackageGameWindow]::ShowWindow($taskGame.MainWindowHandle,9)|Out-Null
if($Fullscreen){[PackageGameWindow]::SetWindowPos($taskGame.MainWindowHandle,[IntPtr]::new(-1),0,0,0,0,0x43)|Out-Null}
else{[PackageGameWindow]::SetWindowPos($taskGame.MainWindowHandle,[IntPtr]::new(-1),30,60,0,0,0x41)|Out-Null}
Start-Sleep -Milliseconds 300
$taskRect=[PackageGameWindow+RECT]::new();[PackageGameWindow]::GetWindowRect($taskGame.MainWindowHandle,[ref]$taskRect)|Out-Null
$taskScreenW=[PackageGameWindow]::GetSystemMetrics(0);$taskScreenH=[PackageGameWindow]::GetSystemMetrics(1)
Add-Type -AssemblyName System.Drawing
$taskDesktop=[Drawing.Bitmap]::new($taskScreenW,$taskScreenH);$taskGraphics=[Drawing.Graphics]::FromImage($taskDesktop)
$taskPng=Join-Path $PSScriptRoot ('launch-'+$Label+'.png')
try{
    $taskGraphics.CopyFromScreen(0,0,0,0,$taskDesktop.Size)
    $taskCrop=[Drawing.Rectangle]::new([Math]::Max(0,$taskRect.L),[Math]::Max(0,$taskRect.T),[Math]::Min($taskRect.R,$taskScreenW)-[Math]::Max(0,$taskRect.L),[Math]::Min($taskRect.B,$taskScreenH)-[Math]::Max(0,$taskRect.T))
    $taskPicture=$taskDesktop.Clone($taskCrop,$taskDesktop.PixelFormat)
    try{$taskPicture.Save($taskPng,[Drawing.Imaging.ImageFormat]::Png)}finally{$taskPicture.Dispose()}
}finally{$taskGraphics.Dispose();$taskDesktop.Dispose()}
$taskResult=@{status='PASS';package=$Package;exe=$taskExe;pid=$taskGame.Id;window=$taskGame.MainWindowTitle;fullscreen=[bool]$Fullscreen;rect=@($taskRect.L,$taskRect.T,$taskRect.R,$taskRect.B);screen=@($taskScreenW,$taskScreenH);screenshot=$taskPng;kept=[bool]$Keep}
$taskResult|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $PSScriptRoot ('launch-'+$Label+'.json')) -Encoding utf8
if(-not $Keep){
    [PackageGameWindow]::PostMessage($taskGame.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
    if(-not $taskGame.WaitForExit(5000)){throw 'Own test game did not close'}
}else{[PackageGameWindow]::SetWindowPos($taskGame.MainWindowHandle,[IntPtr]::new(-2),0,0,0,0,0x13)|Out-Null}
'PACKAGE PC START PASS '+$Package
