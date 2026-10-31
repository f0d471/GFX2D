param(
    [Parameter(Mandatory = $true)][string]$Build,
    [string]$App = 'menu',
    [switch]$Fullscreen,
    [int]$Width = 1600,
    [int]$Height = 1000,
    [string]$InputScript = '',
    [int]$SettleMs = 900,
    [switch]$Stop,
    [switch]$CaptureOnly,
    [switch]$CheckOnly
)

# 原生 Windows 试玩窗的启动、前台化与客户区截帧。
#
# 用途：PC 端逐轮试玩——本工具打开 build_pc.py 产出的 gfx2dGame.exe，把它摆到前台，
# 抓一张客户区 PNG 给审阅者与自己看；进程留在桌面上由人继续操作。修改源码后重跑
# build_pc.py（同一 OBJDIR 是增量）、再用 -Stop 关old、重新打开即下一轮。
#
# 轮次身份：review-session.json 记 exe 路径、SHA256、PID、窗口句柄与最近一张截图。
# 关进程或截帧前都先核对路径，不按 PID 猜；PID 被系统复用时直接判身份不符。
#
# 依赖：Windows 交互式桌面、System.Drawing；exe 由 tools/board/build_pc.py 构建。

$ErrorActionPreference = 'Stop'
$taskBuild = [IO.Path]::GetFullPath($Build)
$taskExe = Join-Path $taskBuild 'gfx2dGame.exe'
$taskSessionPath = Join-Path $taskBuild 'review-session.json'

function Get-ReviewSession {
    if (-not (Test-Path -LiteralPath $taskSessionPath -PathType Leaf)) { return $null }
    Get-Content -LiteralPath $taskSessionPath -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-SameExeProcesses {
    # 只认 exe 路径与本次构建一致的进程；别的构建跑着不算。
    Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -eq $taskExe -and -not $_.HasExited
    }
}

function Stop-ReviewProcess {
    $taskKilled = 0
    foreach ($taskProcess in @(Get-SameExeProcesses)) {
        $taskProcessId = $taskProcess.Id
        Stop-Process -Id $taskProcessId -Force -ErrorAction SilentlyContinue
        $taskKilled++
        Write-Output ('STOP pid ' + $taskProcessId)
    }
    return $taskKilled
}

if (-not (Test-Path -LiteralPath $taskExe -PathType Leaf)) {
    throw ('找不到可执行文件，先运行 build_pc.py：' + $taskExe)
}
$taskIdentity = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash

if ($CheckOnly) {
    Write-Output ('PC REVIEW READY exe=' + $taskExe)
    Write-Output ('sha256=' + $taskIdentity)
    Write-Output ('app=' + $App + ' fullscreen=' + [bool]$Fullscreen + ' size=' + $Width + 'x' + $Height)
    Write-Output ('session=' + $taskSessionPath)
    exit 0
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PcReviewWindow {
 [StructLayout(LayoutKind.Sequential)] public struct RECT {public int L,T,R,B;}
 [StructLayout(LayoutKind.Sequential)] public struct POINT {public int X,Y;}
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr p);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref POINT p);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int w,int t,uint f);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool yes);
 [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
 public static void Activate(IntPtr h) {
   uint process;uint fg=GetWindowThreadProcessId(GetForegroundWindow(),out process),me=GetCurrentThreadId();
   bool joined=fg!=me && AttachThreadInput(me,fg,true);
   try {ShowWindow(h,9);BringWindowToTop(h);SetForegroundWindow(h);}
   finally {if(joined)AttachThreadInput(me,fg,false);}
 }
}
'@
Add-Type -AssemblyName System.Drawing

function Save-ClientShot {
    param([IntPtr]$Handle)
    $taskRect = New-Object PcReviewWindow+RECT
    $taskPoint = New-Object PcReviewWindow+POINT
    if (-not [PcReviewWindow]::GetClientRect($Handle, [ref]$taskRect)) { throw 'GetClientRect 失败' }
    if (-not [PcReviewWindow]::ClientToScreen($Handle, [ref]$taskPoint)) { throw 'ClientToScreen 失败' }
    $taskClientWidth = $taskRect.R - $taskRect.L
    $taskClientHeight = $taskRect.B - $taskRect.T
    if ($taskClientWidth -le 0 -or $taskClientHeight -le 0) { throw '客户区为空，窗口可能最小化' }
    $taskBitmap = New-Object System.Drawing.Bitmap($taskClientWidth, $taskClientHeight)
    $taskGraphics = [System.Drawing.Graphics]::FromImage($taskBitmap)
    $taskShot = Join-Path $taskBuild ('review-' + (Get-Date -Format 'HHmmss-fff') + '.png')
    try {
        $taskGraphics.CopyFromScreen($taskPoint.X, $taskPoint.Y, 0, 0, $taskBitmap.Size)
        $taskGraphics.Flush()
        $taskBitmap.Save($taskShot, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $taskGraphics.Dispose()
        $taskBitmap.Dispose()
    }
    return $taskShot
}

$taskPriorDpi = [PcReviewWindow]::SetThreadDpiAwarenessContext([IntPtr](-4))
try {
    if ($Stop -and -not $CaptureOnly) {
        $taskKilled = Stop-ReviewProcess
        Write-Output ('PC REVIEW STOPPED count=' + $taskKilled)
        exit 0
    }

    if ($CaptureOnly) {
        $taskSession = Get-ReviewSession
        if (-not $taskSession) { throw ('没有会话记录，先不带 -CaptureOnly 打开：' + $taskSessionPath) }
        $taskProcess = Get-Process -Id $taskSession.pid -ErrorAction SilentlyContinue
        if (-not $taskProcess) { throw ('会话进程已退出 pid=' + $taskSession.pid) }
        if ($taskProcess.Path -ne $taskExe) { throw '会话进程路径与本次构建不一致，先重新打开' }
        if ($taskProcess.HasExited) { throw ('会话进程已退出 pid=' + $taskSession.pid) }
    } else {
        [void](Stop-ReviewProcess)
        $env:GFX2D_ASSETS_PAK = Join-Path $taskBuild 'assets.pak'
        $env:GFX2D_FULLSCREEN = if ($Fullscreen) { '1' } else { '0' }
        $env:GFX2D_ASPECT = 'auto'
        $env:GFX2D_SCALE_FILTER = 'linear'
        $env:GFX2D_UNPACED = '0'
        $env:SDL_VIDEODRIVER = 'windows'
        $taskStamp = Get-Date -Format 'HHmmss'
        $taskStdout = Join-Path $taskBuild ('review-' + $taskStamp + '.stdout.log')
        $taskStderr = Join-Path $taskBuild ('review-' + $taskStamp + '.stderr.log')
        $taskArgs = @('--app', $App)
        if ($InputScript) { $taskArgs += @('--input-script', $InputScript) }
        $taskProcess = Start-Process -FilePath $taskExe -ArgumentList $taskArgs `
            -WorkingDirectory $taskBuild -WindowStyle Normal -PassThru `
            -RedirectStandardOutput $taskStdout -RedirectStandardError $taskStderr
        $taskSession = [ordered]@{
            time = (Get-Date -Format o); pid = $taskProcess.Id; exe = $taskExe
            sha256 = $taskIdentity; app = $App; fullscreen = [bool]$Fullscreen
            width = $Width; height = $Height; input_script = $InputScript
            window_title = ''; handle = 0; visible = $false; foreground = $false
            responding = $false; screenshot = $null; stdout = $taskStdout; stderr = $taskStderr
        }
        for ($taskTry = 0; $taskTry -lt 120; $taskTry++) {
            $taskProcess.Refresh()
            if ($taskProcess.HasExited) { throw ('程序启动后退出，退出码 ' + $taskProcess.ExitCode) }
            if ($taskProcess.MainWindowHandle -ne 0) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($taskProcess.MainWindowHandle -eq 0) { throw '拿不到窗口句柄：请在交互式桌面上运行，不要用无窗口会话' }
        $taskHandle = $taskProcess.MainWindowHandle
        if (-not $Fullscreen) {
            [void][PcReviewWindow]::SetWindowPos($taskHandle, [IntPtr]::Zero, 60, 60, $Width, $Height, 0x0040)
        }
        $taskSession.handle = $taskHandle.ToInt64()
    }

    $taskHandle = [IntPtr]$taskSession.handle
    if ($taskHandle -eq 0) { $taskHandle = $taskProcess.MainWindowHandle }
    if ([PcReviewWindow]::IsIconic($taskHandle)) { [void][PcReviewWindow]::ShowWindow($taskHandle, 9); Start-Sleep -Milliseconds 200 }
    [PcReviewWindow]::Activate($taskHandle)
    Start-Sleep -Milliseconds $SettleMs
    $taskForeground = [PcReviewWindow]::GetForegroundWindow() -eq $taskHandle
    if (-not $taskForeground) { Write-Output 'WARN 窗口未抢占前台，截图仍是该窗口内容，键盘输入不会转发到这个窗口' }
    $taskShot = Save-ClientShot -Handle $taskHandle
    $taskProcess.Refresh()
    $taskSession.window_title = $taskProcess.MainWindowTitle
    $taskSession.visible = [PcReviewWindow]::IsWindowVisible($taskHandle)
    $taskSession.foreground = $taskForeground
    $taskSession.responding = $taskProcess.Responding
    $taskSession.screenshot = $taskShot
    $taskSession | ConvertTo-Json | Set-Content -LiteralPath $taskSessionPath -Encoding UTF8
    Write-Output ('PC REVIEW ' + $(if ($CaptureOnly) { 'CAPTURE' } else { 'OPEN' }) + ' PASS')
    $taskSession | ConvertTo-Json
} finally {
    [void][PcReviewWindow]::SetThreadDpiAwarenessContext($taskPriorDpi)
}
