param(
    [string]$EfinityHome = $env:EFINITY_HOME,
    [string]$Port = 'COM11',
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$taskMain = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$taskControl = Join-Path $PSScriptRoot 'game_control.py'
$taskOutput = Join-Path $taskMain 'sw/apps/console/build_control'
$taskCandidates = [Collections.Generic.List[string]]::new()

if ($EfinityHome) {
    $taskCandidates.Add($EfinityHome)
} else {
    if ($env:PYTHONHOME) {
        $taskCandidates.Add((Split-Path -Parent $env:PYTHONHOME))
    }
    $taskEfxCommand = Get-Command 'efx_run.bat' -ErrorAction SilentlyContinue
    if ($taskEfxCommand) {
        $taskCandidates.Add((Split-Path -Parent (Split-Path -Parent $taskEfxCommand.Source)))
    }
    foreach ($taskDrive in Get-PSDrive -PSProvider FileSystem) {
        $taskInstallRoot = Join-Path $taskDrive.Root 'Efinity'
        if ([IO.Directory]::Exists($taskInstallRoot)) {
            foreach ($taskInstall in Get-ChildItem -LiteralPath $taskInstallRoot -Directory | Sort-Object Name -Descending) {
                $taskCandidates.Add($taskInstall.FullName)
            }
        }
    }
}

$taskPython = $null
foreach ($taskCandidate in $taskCandidates) {
    $taskExecutable = Join-Path $taskCandidate 'python311/bin/python.exe'
    if (Test-Path -LiteralPath $taskExecutable -PathType Leaf) {
        $taskPython = [IO.Path]::GetFullPath($taskExecutable)
        break
    }
}
if (-not $taskPython) {
    throw '未找到 Efinity 自带的 Python。通过 -EfinityHome 指定 Efinity 安装目录。'
}

$taskSavedEnv = @{}
foreach ($taskName in @('PYTHONHOME', 'PYTHONIOENCODING', 'PYTHONDONTWRITEBYTECODE')) {
    $taskSavedEnv[$taskName] = [Environment]::GetEnvironmentVariable($taskName, 'Process')
}
try {
    $env:PYTHONHOME = Split-Path -Parent (Split-Path -Parent $taskPython)
    $env:PYTHONIOENCODING = 'utf-8'
    $env:PYTHONDONTWRITEBYTECODE = '1'
    if ($CheckOnly) {
        & $taskPython -B $taskControl --help
    } else {
        New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
        $taskLog = Join-Path $taskOutput ('play_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.log')
        $taskStatus = Join-Path $taskOutput 'panel_status.json'
        & $taskPython -B $taskControl --port $Port --ensure-hw --log $taskLog --status-file $taskStatus
    }
    if ($LASTEXITCODE -ne 0) {
        throw ('游戏控制面板退出码：' + $LASTEXITCODE)
    }
} finally {
    foreach ($taskName in $taskSavedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($taskName, $taskSavedEnv[$taskName], 'Process')
    }
}
