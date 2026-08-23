#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1")

function Get-HybridFileHash {
    param([Parameter(Mandatory=$true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Write-HybridInstallJournal {
    param([Parameter(Mandatory=$true)][string]$JournalPath,[Parameter(Mandatory=$true)][psobject]$Journal)
    Write-JCodeMunchAtomicText -Path $JournalPath -Text ($Journal|ConvertTo-Json -Depth 20) -Secret
}

function Enter-HybridInstallLock {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$StateRoot)
    $root=[IO.Path]::GetFullPath($StateRoot)
    New-Item -ItemType Directory -Path $root -Force|Out-Null
    Set-JCodeMunchOwnerAcl -Path $root
    $path=Join-Path $root "install.lock"
    try{$stream=[IO.File]::Open($path,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}
    catch [IO.IOException]{throw "Another hybrid install or rollback owns the exclusive transaction lock: $path"}
    try{
        if([OperatingSystem]::IsWindows()){Set-JCodeMunchOwnerAcl -Path $path -File}
        $payload=[Text.Encoding]::UTF8.GetBytes(([ordered]@{pid=$PID;acquired_at=[DateTime]::UtcNow.ToString("o")}|ConvertTo-Json -Compress))
        $stream.SetLength(0);$stream.Write($payload,0,$payload.Length);$stream.Flush($true)
        return $stream
    }catch{$stream.Dispose();throw}
}

function Exit-HybridInstallLock {
    [CmdletBinding()]
    param([AllowNull()][IO.FileStream]$Lock)
    if($null-ne$Lock){$Lock.Dispose()}
}

function New-HybridInstallJournal {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$StateRoot,
        [Parameter(Mandatory=$true)][string]$JournalPath,
        [Parameter(Mandatory=$true)][string[]]$Paths,
        [string[]]$DirectoryPaths=@(),
        [string]$BinaryTarget,
        [hashtable]$ActivationEnvironment=@{}
    )
    $transactionId=[Guid]::NewGuid().ToString("N")
    $backupRoot=Join-Path ([IO.Path]::GetFullPath($StateRoot)) ("transactions\$transactionId")
    New-Item -ItemType Directory -Path $backupRoot -Force|Out-Null
    Set-JCodeMunchOwnerAcl -Path $backupRoot
    $entries=[Collections.Generic.List[object]]::new()
    $index=0
    $resolvedPaths=@($Paths|Where-Object{-not[string]::IsNullOrWhiteSpace($_)}|ForEach-Object{[IO.Path]::GetFullPath($_)}|Sort-Object -Unique)
    $binaryTargetFull=if([string]::IsNullOrWhiteSpace($BinaryTarget)){$null}else{[IO.Path]::GetFullPath($BinaryTarget)}
    if($null-ne$binaryTargetFull-and@($resolvedPaths|Where-Object{[string]::Equals($_,$binaryTargetFull,[StringComparison]::OrdinalIgnoreCase)}).Count-ne1){throw "BinaryTarget must identify exactly one journal file target."}
    foreach($path in $resolvedPaths){
        if(Test-Path -LiteralPath $path -PathType Container){throw "Transaction target is a directory, not a file: $path"}
        $existed=Test-Path -LiteralPath $path -PathType Leaf
        $backupPath=$null;$hash=$null;$sddl=$null;$securityDescriptor=$null
        if($existed){
            $backupPath=Join-Path $backupRoot ("{0:D5}.bin" -f $index)
            [IO.File]::Copy($path,$backupPath,$false)
            Set-JCodeMunchOwnerAcl -Path $backupPath -File
            $hash=Get-HybridFileHash $backupPath
            if([OperatingSystem]::IsWindows()){
                $securityDescriptor=Get-JCodeMunchSecurityDescriptor -Path $path
                try{$sddl=(Get-Acl -LiteralPath $path -ErrorAction Stop).Sddl}catch{}
            }
        }
        $entries.Add([ordered]@{entry_type="file";path=$path;existed=$existed;backup_path=$backupPath;content_sha256=$hash;acl_sddl=$sddl;security_descriptor_base64=$securityDescriptor})
        $index++
    }
    foreach($path in @($DirectoryPaths|Where-Object{-not[string]::IsNullOrWhiteSpace($_)}|ForEach-Object{[IO.Path]::GetFullPath($_)}|Sort-Object -Unique)){
        if(Test-Path -LiteralPath $path -PathType Leaf){throw "Transaction directory target is a file: $path"}
        $existed=Test-Path -LiteralPath $path -PathType Container
        $sddl=$null;$securityDescriptor=$null
        if($existed-and[OperatingSystem]::IsWindows()){
            $securityDescriptor=Get-JCodeMunchSecurityDescriptor -Path $path
            try{$sddl=(Get-Acl -LiteralPath $path -ErrorAction Stop).Sddl}catch{}
        }
        $entries.Add([ordered]@{entry_type="directory";path=$path;existed=$existed;backup_path=$null;content_sha256=$null;acl_sddl=$sddl;security_descriptor_base64=$securityDescriptor})
    }
    $journal=[pscustomobject][ordered]@{
        schema_version=2
        product="codebase-memory-mcp-hybrid"
        transaction_id=$transactionId
        status="prepared"
        backup_root=$backupRoot
        binary_target=$binaryTargetFull
        activation_environment=[ordered]@{
            HOME=if($ActivationEnvironment.ContainsKey("HOME")){[string]$ActivationEnvironment["HOME"]}else{""}
            USERPROFILE=if($ActivationEnvironment.ContainsKey("USERPROFILE")){[string]$ActivationEnvironment["USERPROFILE"]}else{""}
            CODEX_HOME=if($ActivationEnvironment.ContainsKey("CODEX_HOME")){[string]$ActivationEnvironment["CODEX_HOME"]}else{""}
            CLAUDE_CONFIG_DIR=if($ActivationEnvironment.ContainsKey("CLAUDE_CONFIG_DIR")){[string]$ActivationEnvironment["CLAUDE_CONFIG_DIR"]}else{""}
        }
        entries=@($entries)
        published_runtime_paths=@()
        prepared_at=[DateTime]::UtcNow.ToString("o")
    }
    Write-HybridInstallJournal -JournalPath $JournalPath -Journal $journal
    return $journal
}

function Set-HybridInstallJournalStatus {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$JournalPath,
        [Parameter(Mandatory=$true)][ValidateSet("prepared","applied","rolled_back")][string]$Status,
        [string[]]$PublishedRuntimePaths=@()
    )
    $journal=Get-Content -LiteralPath $JournalPath -Raw -Encoding utf8|ConvertFrom-Json
    $journal.status=$Status
    if($PublishedRuntimePaths.Count-gt0){$journal.published_runtime_paths=@($PublishedRuntimePaths|ForEach-Object{[IO.Path]::GetFullPath($_)}|Sort-Object -Unique)}
    $journal|Add-Member -NotePropertyName ("${Status}_at") -NotePropertyValue ([DateTime]::UtcNow.ToString("o")) -Force
    Write-HybridInstallJournal -JournalPath $JournalPath -Journal $journal
    return $journal
}

function Restore-HybridInstallJournal {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$JournalPath,[string[]]$SkipPaths=@())
    if(-not(Test-Path -LiteralPath $JournalPath -PathType Leaf)){throw "Hybrid install journal is missing: $JournalPath"}
    $journal=Get-Content -LiteralPath $JournalPath -Raw -Encoding utf8|ConvertFrom-Json
    if([int]$journal.schema_version-ne2-or[string]$journal.product-ne"codebase-memory-mcp-hybrid"){throw "Unsupported hybrid install journal."}
    $skipped=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($path in @($SkipPaths|Where-Object{-not[string]::IsNullOrWhiteSpace($_)})){[void]$skipped.Add([IO.Path]::GetFullPath($path))}
    $restored=0;$deleted=0;$failures=[Collections.Generic.List[object]]::new()
    foreach($entry in @($journal.entries)){
        $target=[IO.Path]::GetFullPath([string]$entry.path)
        if($skipped.Contains($target)){continue}
        try{
            if([string]$entry.entry_type-eq"directory"){
                if([bool]$entry.existed){
                    if(Test-Path -LiteralPath $target -PathType Leaf){throw "Restored directory target is occupied by a file."}
                    if(-not(Test-Path -LiteralPath $target -PathType Container)){New-Item -ItemType Directory -Path $target -Force|Out-Null}
                    if([OperatingSystem]::IsWindows()){
                        if([string]::IsNullOrWhiteSpace([string]$entry.security_descriptor_base64)){throw "Recorded directory security descriptor is missing."}
                        Set-JCodeMunchSecurityDescriptor -Path $target -DescriptorBase64 ([string]$entry.security_descriptor_base64)
                        if(-not(Test-JCodeMunchSecurityDescriptorEquivalent -ExpectedBase64 ([string]$entry.security_descriptor_base64) -ActualBase64 (Get-JCodeMunchSecurityDescriptor -Path $target))){throw "Restored directory security descriptor verification failed."}
                    }
                    $restored++
                }elseif(Test-Path -LiteralPath $target -PathType Container){
                    if(@(Get-ChildItem -LiteralPath $target -Force).Count-gt0){throw "New transaction directory is not empty and was preserved."}
                    Remove-Item -LiteralPath $target -Force -ErrorAction Stop;$deleted++
                }
                continue
            }
            if([bool]$entry.existed){
                $backup=[IO.Path]::GetFullPath([string]$entry.backup_path)
                if(-not(Test-Path -LiteralPath $backup -PathType Leaf)){throw "Backup file is missing."}
                if((Get-HybridFileHash $backup)-ne[string]$entry.content_sha256){throw "Backup integrity mismatch."}
                $parent=Split-Path -Parent $target
                if(-not(Test-Path -LiteralPath $parent -PathType Container)){New-Item -ItemType Directory -Path $parent -Force|Out-Null}
                $temp=Join-Path $parent (".{0}.{1}.hybrid-restore.tmp" -f [IO.Path]::GetFileName($target),[Guid]::NewGuid().ToString("N"))
                $replacementBackup=Join-Path $parent (".{0}.{1}.hybrid-restore.bak" -f [IO.Path]::GetFileName($target),[Guid]::NewGuid().ToString("N"))
                try{
                    [IO.File]::Copy($backup,$temp,$false)
                    $preserved=$false
                    if(Test-Path -LiteralPath $target -PathType Leaf){
                        try{[IO.File]::Replace($temp,$target,$replacementBackup,$true);$preserved=$true;Remove-Item -LiteralPath $replacementBackup -Force -ErrorAction SilentlyContinue}catch [PlatformNotSupportedException]{[IO.File]::Move($temp,$target,$true)}
                    }else{[IO.File]::Move($temp,$target)}
                }finally{if(Test-Path -LiteralPath $temp -PathType Leaf){Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue};if(Test-Path -LiteralPath $replacementBackup -PathType Leaf){Remove-Item -LiteralPath $replacementBackup -Force -ErrorAction SilentlyContinue}}
                if((Get-HybridFileHash $target)-ne[string]$entry.content_sha256){throw "Restored content verification failed."}
                if([OperatingSystem]::IsWindows()-and-not[string]::IsNullOrWhiteSpace([string]$entry.security_descriptor_base64)){
                    Set-JCodeMunchSecurityDescriptor -Path $target -DescriptorBase64 ([string]$entry.security_descriptor_base64)
                    if(-not(Test-JCodeMunchSecurityDescriptorEquivalent -ExpectedBase64 ([string]$entry.security_descriptor_base64) -ActualBase64 (Get-JCodeMunchSecurityDescriptor -Path $target))){throw "Restored security descriptor verification failed."}
                }
                $restored++
            }elseif(Test-Path -LiteralPath $target -PathType Leaf){
                Remove-Item -LiteralPath $target -Force -ErrorAction Stop
                if(Test-Path -LiteralPath $target){throw "New transaction file could not be removed."}
                $deleted++
            }
        }catch{$failures.Add([ordered]@{path=$target;message=$_.Exception.Message})}
    }
    if($failures.Count-gt0){
        $message=($failures|ForEach-Object{"$($_.path): $($_.message)"})-join"; "
        throw "Hybrid install rollback failed verification: $message"
    }
    $journal.status="rolled_back"
    $journal|Add-Member -NotePropertyName rolled_back_at -NotePropertyValue ([DateTime]::UtcNow.ToString("o")) -Force
    Write-HybridInstallJournal -JournalPath $JournalPath -Journal $journal
    return [pscustomobject]@{transaction_id=[string]$journal.transaction_id;restored=$restored;deleted=$deleted;failed=0;status="rolled_back"}
}

Export-ModuleMember -Function Enter-HybridInstallLock,Exit-HybridInstallLock,New-HybridInstallJournal,Set-HybridInstallJournalStatus,Restore-HybridInstallJournal
