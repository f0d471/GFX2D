param([string]$Exe = "$PSScriptRoot/windows-play/gfx2dGame.exe",[string]$Label='candidate',[switch]$Keep,[switch]$Scenes)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing,System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WideLiveWindow {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int w,int z,uint f);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,IntPtr p);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 public static void Key(IntPtr h,int vk,int scan,bool up) { PostMessage(h,up?0x101u:0x100u,new IntPtr(vk),new IntPtr((up?0xc0000001L:1L)|((long)scan<<16))); }
 [StructLayout(LayoutKind.Sequential)] public struct MouseData { public int x,y; public uint data,flags,time; public IntPtr extra; }
 [StructLayout(LayoutKind.Sequential)] public struct KeyData { public ushort vk,scan; public uint flags,time; public IntPtr extra; }
 [StructLayout(LayoutKind.Explicit)] public struct InputData { [FieldOffset(0)] public MouseData mouse; [FieldOffset(0)] public KeyData key; }
 [StructLayout(LayoutKind.Sequential)] public struct InputEvent { public uint type; public InputData data; }
 [DllImport("user32.dll",SetLastError=true)] public static extern uint SendInput(uint count,InputEvent[] inputs,int size);
 public static void Scan(ushort scan,bool up) { var input=new InputEvent();input.type=1;input.data.key.scan=scan;input.data.key.flags=8u|(up?2u:0u); if(SendInput(1,new[]{input},Marshal.SizeOf(typeof(InputEvent)))!=1)throw new InvalidOperationException("SendInput failed"); }
}
'@
[WideLiveWindow]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))|Out-Null
$taskOutput=Join-Path $PSScriptRoot ('live-'+$Label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $taskOutput|Out-Null
$taskExecutable=[IO.Path]::GetFullPath($Exe)
$env:GFX2D_ASSETS_PAK=Join-Path (Split-Path -Parent $taskExecutable) 'assets.pak'
$env:GFX2D_SCALE_FILTER='linear';$env:GFX2D_FULLSCREEN='0';$env:GFX2D_ASPECT='auto'
$taskGame=Start-Process -FilePath $taskExecutable -WorkingDirectory (Split-Path -Parent $taskExecutable) -WindowStyle Normal -PassThru -RedirectStandardOutput (Join-Path $taskOutput 'game.log') -RedirectStandardError (Join-Path $taskOutput 'game.stderr.log')
trap {
    if($taskGame -and -not $Keep -and -not $taskGame.HasExited) {
        [WideLiveWindow]::PostMessage($taskGame.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
        [void]$taskGame.WaitForExit(3000)
    }
    throw
}
for($taskTry=0;$taskTry -lt 80;$taskTry++) {
    Start-Sleep -Milliseconds 100;$taskGame.Refresh()
    if($taskGame.HasExited){throw 'Candidate exited before showing a window'}
    if($taskGame.MainWindowHandle -ne 0){break}
}
$taskWindow=$taskGame.MainWindowHandle
if($taskWindow -eq 0){throw 'Candidate has no visible window'}
function Set-GameForeground {
    for($taskFocusTry=0;$taskFocusTry -lt 5;$taskFocusTry++) {
        $taskFore=[WideLiveWindow]::GetWindowThreadProcessId([WideLiveWindow]::GetForegroundWindow(),[IntPtr]::Zero)
        $taskGui=[WideLiveWindow]::GetWindowThreadProcessId($taskWindow,[IntPtr]::Zero)
        $taskThread=[WideLiveWindow]::GetCurrentThreadId()
        $taskJoined=[Collections.Generic.List[uint32]]::new()
        foreach($taskOther in @($taskFore,$taskGui)|Select-Object -Unique) {
            if($taskOther -ne $taskThread -and [WideLiveWindow]::AttachThreadInput($taskThread,$taskOther,$true)){$taskJoined.Add($taskOther)}
        }
        try {
            [WideLiveWindow]::ShowWindow($taskWindow,9)|Out-Null
            [WideLiveWindow]::SetWindowPos($taskWindow,[IntPtr]::new(-1),0,0,0,0,0x43)|Out-Null
            [WideLiveWindow]::SetForegroundWindow($taskWindow)|Out-Null
            [WideLiveWindow]::SetActiveWindow($taskWindow)|Out-Null
            [WideLiveWindow]::SetFocus($taskWindow)|Out-Null
        } finally {foreach($taskOther in $taskJoined){[WideLiveWindow]::AttachThreadInput($taskThread,$taskOther,$false)|Out-Null}}
        Start-Sleep -Milliseconds 100
        if([WideLiveWindow]::GetForegroundWindow() -eq $taskWindow){return}
    }
    throw 'Game is not foreground'
}
function Save-GameImage([string]$Name) {
    Set-GameForeground
    $taskShotRect=[WideLiveWindow+RECT]::new();[WideLiveWindow]::GetWindowRect($taskWindow,[ref]$taskShotRect)|Out-Null
    $taskShot=[Drawing.Bitmap]::new(($taskShotRect.R-$taskShotRect.L),($taskShotRect.B-$taskShotRect.T))
    $taskPainter=[Drawing.Graphics]::FromImage($taskShot)
    try{$taskPainter.CopyFromScreen($taskShotRect.L,$taskShotRect.T,0,0,$taskShot.Size);$taskShot.Save((Join-Path $taskOutput $Name),[Drawing.Imaging.ImageFormat]::Png)}finally{$taskPainter.Dispose();$taskShot.Dispose()}
}
function Press-GameKey([int]$VirtualKey,[int]$Scan) {
    Set-GameForeground
    try {
        if($VirtualKey -eq 0x10){[WideLiveWindow]::Scan($Scan,$false)}else{[WideLiveWindow]::Key($taskWindow,$VirtualKey,$Scan,$false)}
        Start-Sleep -Milliseconds 180
    } finally {
        if($VirtualKey -eq 0x10){[WideLiveWindow]::Scan($Scan,$true)}else{[WideLiveWindow]::Key($taskWindow,$VirtualKey,$Scan,$true)}
    }
    Start-Sleep -Milliseconds 1300
}
if($Scenes) {
    [WideLiveWindow]::SetWindowPos($taskWindow,[IntPtr]::new(-1),40,60,1940,600,0x40)|Out-Null
    Start-Sleep -Milliseconds 150
    $taskOuter=[WideLiveWindow+RECT]::new();$taskClient=[WideLiveWindow+RECT]::new()
    [WideLiveWindow]::GetWindowRect($taskWindow,[ref]$taskOuter)|Out-Null;[WideLiveWindow]::GetClientRect($taskWindow,[ref]$taskClient)|Out-Null
    [WideLiveWindow]::SetWindowPos($taskWindow,[IntPtr]::new(-1),40,60,(1920+$taskOuter.R-$taskOuter.L-$taskClient.R),(540+$taskOuter.B-$taskOuter.T-$taskClient.B),0x40)|Out-Null
    Start-Sleep -Seconds 3;Save-GameImage 'wide-corridor-l2.png'
    Press-GameKey 0x10 0x2a;Save-GameImage 'wide-corridor-l1.png'
    Press-GameKey 0x10 0x2a;Press-GameKey 0x34 0x05;Save-GameImage 'wide-cup.png'
    Press-GameKey 0x34 0x05
}
$taskResults=@();$taskIndex=0
foreach($taskScreen in [Windows.Forms.Screen]::AllScreens) {
    $taskBounds=$taskScreen.Bounds
    [WideLiveWindow]::ShowWindow($taskWindow,9)|Out-Null
    [WideLiveWindow]::SetWindowPos($taskWindow,[IntPtr]::new(-1),($taskBounds.X+40),($taskBounds.Y+40),1000,700,0x40)|Out-Null
    [WideLiveWindow]::SetForegroundWindow($taskWindow)|Out-Null
    Start-Sleep -Milliseconds 300
    [WideLiveWindow]::PostMessage($taskWindow,0x100,[IntPtr]::new(0x7a),[IntPtr]::new(0x570001))|Out-Null
    [WideLiveWindow]::PostMessage($taskWindow,0x101,[IntPtr]::new(0x7a),[IntPtr]::new([long]0xc0570001))|Out-Null
    Start-Sleep -Seconds 3
    Set-GameForeground
    $taskRect=[WideLiveWindow+RECT]::new()
    [WideLiveWindow]::GetWindowRect($taskWindow,[ref]$taskRect)|Out-Null
    if($taskRect.L -ne $taskBounds.Left -or $taskRect.T -ne $taskBounds.Top -or $taskRect.R -ne $taskBounds.Right -or $taskRect.B -ne $taskBounds.Bottom){throw 'Fullscreen does not match selected monitor'}
    $taskPng=Join-Path $taskOutput ('display-'+$taskIndex+'.png')
    $taskBitmap=[Drawing.Bitmap]::new($taskBounds.Width,$taskBounds.Height)
    $taskGraphics=[Drawing.Graphics]::FromImage($taskBitmap)
    try{$taskGraphics.CopyFromScreen($taskRect.L,$taskRect.T,0,0,$taskBitmap.Size);$taskBitmap.Save($taskPng,[Drawing.Imaging.ImageFormat]::Png)}finally{$taskGraphics.Dispose();$taskBitmap.Dispose()}
    $taskResults+=@{display=$taskIndex;size=@($taskBounds.Width,$taskBounds.Height);dpi=[WideLiveWindow]::GetDpiForWindow($taskWindow);rect=@($taskRect.L,$taskRect.T,$taskRect.R,$taskRect.B);screenshot=$taskPng}
    $taskIndex++
    [WideLiveWindow]::PostMessage($taskWindow,0x100,[IntPtr]::new(0x1b),[IntPtr]::new(0x10001))|Out-Null
    [WideLiveWindow]::PostMessage($taskWindow,0x101,[IntPtr]::new(0x1b),[IntPtr]::new([long]0xc0010001))|Out-Null
    Start-Sleep -Milliseconds 300
}
if($Keep) {
    [WideLiveWindow]::PostMessage($taskWindow,0x100,[IntPtr]::new(0x7a),[IntPtr]::new(0x570001))|Out-Null
    [WideLiveWindow]::PostMessage($taskWindow,0x101,[IntPtr]::new(0x7a),[IntPtr]::new([long]0xc0570001))|Out-Null
    [WideLiveWindow]::SetWindowPos($taskWindow,[IntPtr]::new(-2),0,0,0,0,0x13)|Out-Null
} else {
    [WideLiveWindow]::PostMessage($taskWindow,0x10,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
    if(-not $taskGame.WaitForExit(5000)){throw 'Candidate did not close cleanly'}
    if($taskGame.ExitCode -ne 0){throw 'Candidate game failed'}
    if($Scenes) {
        $taskBody=Get-Content -LiteralPath (Join-Path $taskOutput 'game.log') -Raw
        if($taskBody -notmatch 'r4 enter ladder/hw 1 0' -or $taskBody -notmatch '(?m)^R4STAT \d+ \d+ \d+ \d+ 0 1\s*$' -or $taskBody -notmatch '(?m)^fail 0\s*$'){throw 'Scene switch/live completion was not confirmed'}
    }
}
$taskRecord=@{pid=$taskGame.Id;exe=$taskExecutable;displays=$taskResults;kept=[bool]$Keep;status='PASS'}
$taskRecord|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $taskOutput 'result.json') -Encoding utf8
$taskRecord|ConvertTo-Json -Depth 6
