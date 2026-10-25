param([string]$Package = 'E:\Workspace\FPGA\Efinix\Main\bit\board_release_game_wide_20261025')
$ErrorActionPreference='Stop'
$taskExe=Join-Path $Package 'pc/gfx2dGame.exe'
$taskOutput=Join-Path $PSScriptRoot ('entries-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $taskOutput|Out-Null
function Get-EntryHash([string]$Path) {
    $taskHasher=[Security.Cryptography.SHA256]::Create();$taskStream=[IO.File]::OpenRead($Path)
    try{return [BitConverter]::ToString($taskHasher.ComputeHash($taskStream)).Replace('-','').ToLowerInvariant()}finally{$taskStream.Dispose();$taskHasher.Dispose()}
}
$taskManifestHash=Get-EntryHash (Join-Path $Package 'manifest.json')
$taskExpectedExeHash=Get-EntryHash $taskExe
$taskResults=@()
foreach($taskCase in @(
    @{name='ps7-fixed';shell=(Get-Command pwsh).Source;fixed=$true;keep=$false},
    @{name='ps51-auto';shell='C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe';fixed=$false;keep=$true}
)) {
    $taskExisting=@(Get-Process -Name gfx2dGame -ErrorAction SilentlyContinue|Where-Object {$_.Path -eq $taskExe}|ForEach-Object {$_.Id})
    $taskArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $Package 'Run-PC.ps1'))
    if($taskCase.fixed){$taskArgs+='-FixedAspect'}
    $taskLauncher=Start-Process -FilePath $taskCase.shell -ArgumentList $taskArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $taskOutput ($taskCase.name+'.log')) -RedirectStandardError (Join-Path $taskOutput ($taskCase.name+'.stderr.log'))
    if(-not $taskLauncher.WaitForExit(15000) -or $taskLauncher.ExitCode -ne 0){throw ('Package entry failed '+$taskCase.name)}
    $taskNew=$null
    for($taskAttempt=0;$taskAttempt -lt 60;$taskAttempt++) {
        $taskNew=Get-Process -Name gfx2dGame -ErrorAction SilentlyContinue|Where-Object {$_.Path -eq $taskExe -and $_.Id -notin $taskExisting}|Select-Object -First 1
        if($taskNew -and $taskNew.MainWindowHandle -ne 0){break}
        Start-Sleep -Milliseconds 100
    }
    if(-not $taskNew -or $taskNew.MainWindowHandle -eq 0){throw ('No visible package game '+$taskCase.name)}
    $taskReviewArgs=@{Exe=$taskExe;Label=('package-'+$taskCase.name);ProcessId=$taskNew.Id}
    if($taskCase.keep){$taskReviewArgs.Keep=$true}
    & (Join-Path $PSScriptRoot 'verify_live.ps1') @taskReviewArgs
    if((Get-EntryHash $taskExe) -ne $taskExpectedExeHash -or (Get-EntryHash (Join-Path $Package 'manifest.json')) -ne $taskManifestHash){throw 'Package identity changed'}
    $taskResults+=@{name=$taskCase.name;status='PASS';pid=$taskNew.Id;kept=$taskCase.keep}
}
$taskResult=@{status='PASS';manifest_sha256=$taskManifestHash;exe_sha256=$taskExpectedExeHash;cases=$taskResults}
$taskResult|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $taskOutput 'result.json') -Encoding utf8
$taskResult|ConvertTo-Json -Depth 5
