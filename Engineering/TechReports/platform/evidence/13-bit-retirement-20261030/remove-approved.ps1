param([switch]$Execute,[string]$RepositoryRoot)
$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..\..\..')).ProviderPath
$taskBit=(Resolve-Path -LiteralPath (Join-Path $taskRepo 'Main\bit')).ProviderPath.TrimEnd('\')
if($Execute -and -not $RepositoryRoot){throw 'Execute requires an explicitly named repository root'}
if($RepositoryRoot -and (Resolve-Path -LiteralPath $RepositoryRoot).ProviderPath.TrimEnd('\') -ne $taskRepo.TrimEnd('\')){throw 'Explicit repository root does not match this cleanup record'}
if(-not $taskBit.StartsWith($taskRepo.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Target is outside the named repository'}
foreach($taskAncestor in @($taskRepo,(Join-Path $taskRepo 'Main'),$taskBit)) {
    if((Get-Item -LiteralPath $taskAncestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse point in the target ancestry'}
}
$taskApproved=@('board_release_20261015','board_release_console_20261028','board_release_game_current_20261024',
    'board_release_game_next_20261024','board_release_game_wide_20261025','board_release_r3_20261023','board_release_r4_visual_20261024')
$taskKeep=@('snapshots','official','flash_backup','board_release_console_2d_20261030','board_release_console_full_20261030')
$taskInventory=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'inventory-before.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$taskRetention=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'retention.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if(-not $taskRetention.records_verified -or -not $taskRetention.source_reconstruction_verified){throw 'Historical retention is not verified'}
if(@($taskInventory.retired.PSObject.Properties.Name | Where-Object {$_ -notin $taskApproved}).Count -or @($taskInventory.retired.PSObject.Properties).Count -ne 7){throw 'Inventory scope differs from the approved seven directories'}

function Get-CheckedFiles([string]$Directory,[switch]$SkipRuntime) {
    $taskFolder=Get-Item -LiteralPath $Directory -Force
    if($taskFolder.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse-point root'}
    $taskStack=New-Object 'System.Collections.Generic.Stack[string]';$taskStack.Push($Directory)
    while($taskStack.Count) {
        foreach($taskEntry in Get-ChildItem -LiteralPath ($taskStack.Pop()) -Force) {
            if($taskEntry.Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('Reparse point: '+$taskEntry.FullName)}
            if($taskEntry.PSIsContainer) {
                if($SkipRuntime -and $taskEntry.Parent.FullName -eq $Directory -and $taskEntry.Name -eq 'runtime'){continue}
                $taskStack.Push($taskEntry.FullName)
            } else {$taskEntry}
        }
    }
}
function Assert-Identity([string]$Path,$Identity) {
    $taskFile=Get-Item -LiteralPath $Path -Force
    if($taskFile.Length -ne $Identity.bytes -or (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Identity.sha256){throw ('Identity changed: '+$Path)}
}
function Assert-Tree([string]$Name,$Expected,[switch]$SkipRuntime) {
    $taskDir=(Resolve-Path -LiteralPath (Join-Path $taskBit $Name)).ProviderPath.TrimEnd('\')
    if([IO.Path]::GetDirectoryName($taskDir) -ne $taskBit -or $Name -notin ($taskApproved+$taskKeep)){throw 'Resolved directory is outside the explicit scope'}
    $taskFiles=@(Get-CheckedFiles $taskDir -SkipRuntime:$SkipRuntime)
    $taskProperties=@($Expected.PSObject.Properties)
    if($taskFiles.Count -ne $taskProperties.Count){throw ('File set changed: '+$Name)}
    foreach($taskFile in $taskFiles) {
        $taskRelative=$taskFile.FullName.Substring($taskDir.Length+1).Replace('\','/')
        $taskItem=$Expected.PSObject.Properties[$taskRelative]
        if(-not $taskItem){throw ('Unexpected file: '+$taskFile.FullName)}
        Assert-Identity $taskFile.FullName $taskItem.Value
    }
    return $taskDir
}

# Verify the retained historical records and all reconstructed-source containers.
foreach($taskName in $taskApproved) {
    $taskPackage=$taskRetention.packages.PSObject.Properties[$taskName].Value
    foreach($taskRecord in $taskPackage.records) {
        if($taskRecord.canonical){$taskPath=Join-Path $taskRepo $taskRecord.canonical}
        else {$taskPath=Join-Path $PSScriptRoot $taskRecord.stored}
        Assert-Identity $taskPath $taskRecord
    }
    if($taskPackage.source_delta){Assert-Identity (Join-Path $PSScriptRoot $taskPackage.source_delta.file) $taskPackage.source_delta}
}
$taskTargets=@()
foreach($taskName in $taskApproved){$taskTargets+=Assert-Tree $taskName $taskInventory.retired.PSObject.Properties[$taskName].Value}
foreach($taskName in $taskKeep){[void](Assert-Tree $taskName $taskInventory.retained.PSObject.Properties[$taskName].Value -SkipRuntime)}
$taskTargets | ForEach-Object {Write-Output ('VERIFIED TARGET '+$_)}
if(-not $Execute){Write-Output 'PLAN ONLY; no directories removed';return}

$taskValidation=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'validation.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($taskValidation.status -ne 'PASS' -or $taskValidation.host.status -ne 'PASS' -or $taskValidation.board.status -ne 'PASS'){throw 'Dependency migration checks have not passed'}
$taskSnapshot=Join-Path $taskBit 'snapshots\B7\gfx2d_ti60.bit'
if((Get-FileHash -LiteralPath $taskSnapshot -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskValidation.bit_sha256){throw 'Verified snapshot identity changed'}

# Close only programs whose executable or command line points into a retired package.
$taskStopped=@()
$taskCandidates=Get-CimInstance Win32_Process | Where-Object {$_.Name -in @('gfx2dGame.exe','python.exe','pythonw.exe','openocd.exe')}
foreach($taskProcess in $taskCandidates) {
    foreach($taskDir in $taskTargets) {
        $taskCommand=[string]$taskProcess.CommandLine;$taskExe=[string]$taskProcess.ExecutablePath
        if($taskExe.StartsWith($taskDir+'\',[StringComparison]::OrdinalIgnoreCase) -or
           $taskCommand.IndexOf($taskDir+'\',[StringComparison]::OrdinalIgnoreCase) -ge 0 -or
           $taskCommand.IndexOf($taskDir.Replace('\','/')+'/',[StringComparison]::OrdinalIgnoreCase) -ge 0) {
            Stop-Process -Id $taskProcess.ProcessId
            $taskStopped+=@{pid=$taskProcess.ProcessId;name=$taskProcess.Name;retired_directory=$taskDir};break
        }
    }
}
$taskRemoved=@();$taskBytes=0L
foreach($taskName in $taskApproved) {
    # Resolve and verify again immediately before the irreversible filesystem operation.
    $taskDir=Assert-Tree $taskName $taskInventory.retired.PSObject.Properties[$taskName].Value
    $taskSize=0L;foreach($taskEntry in $taskInventory.retired.PSObject.Properties[$taskName].Value.PSObject.Properties){$taskSize+=[long]$taskEntry.Value.bytes}
    Remove-Item -LiteralPath $taskDir -Recurse -Force
    if(Test-Path -LiteralPath $taskDir){throw ('Removal incomplete: '+$taskDir)}
    $taskBytes+=$taskSize;$taskRemoved+=@{name=$taskName;bytes=$taskSize}
    Write-Output ('REMOVED '+$taskDir)
}
foreach($taskName in $taskKeep){[void](Assert-Tree $taskName $taskInventory.retained.PSObject.Properties[$taskName].Value -SkipRuntime)}
$taskRecord=@{status='PASS';removed=$taskRemoved;removed_bytes=$taskBytes;protected_static_files='byte-identical';retained=$taskKeep;stopped_old_processes=$taskStopped;scope='only seven approved old release directories';flash_written=$false}
$taskRecord | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'deletion.json') -Encoding UTF8
Write-Output ('CLEANUP PASS '+$taskBytes+' bytes removed; protected payloads unchanged')
