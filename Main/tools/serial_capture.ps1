# serial_capture.ps1 —— 只做一件事：读串口并写进日志文件。
#
# 为什么不用 tools/board_run.py 读串口：本机没有可用的 Windows 版 python
# （Efinity 自带的那份缺标准库，没有 encodings）。WSL 里有 pyserial 与串口驱动，
# 但拿不到 Windows 的 COM 口。所以分工是：
#
#   WSL   ：OpenOCD 加载程序（Windows 的 openocd.exe 可从 WSL 直接调用）、烧比特流
#   Windows：本脚本只读串口
#
# 用法（先把本脚本挂到后台，再去加载程序，最后读 <log>）：
#
#   pwsh -File tools/serial_capture.ps1 -Port COM11 -Log <日志> -Seconds 45 [-StopOn done]

param(
    [string]$Port = 'COM11',
    [int]$Baud = 115200,
    [Parameter(Mandatory = $true)][string]$Log,
    [int]$Seconds = 45,
    [string]$StopOn = ''
)

$ErrorActionPreference = 'Stop'

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 200
$sp.Open()

$deadline = (Get-Date).AddSeconds($Seconds)
$all = New-Object System.Collections.Generic.List[byte]

# 日志先建出来，调用方以「文件出现」作为串口已打开的凭据
Set-Content -Path $Log -Value ("# {0} @ {1} 8N1, opened {2}" -f $Port, $Baud, (Get-Date -Format 'HH:mm:ss')) -Encoding utf8

# 逐字节读，不用 BaseStream.Read：那个是阻塞调用，ReadTimeout 对它不生效，
# 采集进程不会按时退出，还会一直占着串口（实测过，第二次采集报 Access denied）。
$chunk = New-Object System.Text.StringBuilder
try {
    while ((Get-Date) -lt $deadline) {
        try {
            $b = $sp.ReadByte()
        } catch [System.TimeoutException] {
            continue
        } catch [System.IO.IOException] {
            break
        }
        if ($b -lt 0) { continue }
        $all.Add([byte]$b)
        [void]$chunk.Append([char]$b)
        if ($chunk.Length -ge 64) {
            [System.IO.File]::AppendAllText($Log, $chunk.ToString())
            [void]$chunk.Clear()
            if ($StopOn -ne '' -and ([System.Text.Encoding]::ASCII.GetString($all.ToArray())).Contains($StopOn)) {
                break
            }
        }
    }
    if ($chunk.Length -gt 0) {
        [System.IO.File]::AppendAllText($Log, $chunk.ToString())
    }
} finally {
    $sp.Close()
    [System.IO.File]::AppendAllText($Log, ("`n# closed {0}, total {1} bytes`n" -f (Get-Date -Format 'HH:mm:ss'), $all.Count))
}

Write-Output ("serial_capture: {0} 字节 -> {1}" -f $all.Count, $Log)
