#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$StateRoot,
    [string]$ConfigRoot,
    [string]$JournalPath,
    [string]$NativeActivator,
    [string[]]$PublishedRuntimePaths=@(),
    [switch]$LockAlreadyHeld
)

Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
if([string]::IsNullOrWhiteSpace($StateRoot)){$StateRoot=Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "CodebaseMemoryMcp\companions\jcodemunch"}
$stateRootFull=[IO.Path]::GetFullPath($StateRoot)
if([string]::IsNullOrWhiteSpace($ConfigRoot)){$ConfigRoot=$stateRootFull}
if([string]::IsNullOrWhiteSpace($JournalPath)){$JournalPath=Join-Path $stateRootFull "install-transaction.json"}
$journalFull=[IO.Path]::GetFullPath($JournalPath)
Import-Module (Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1") -Force
Import-Module (Join-Path $PSScriptRoot "HybridInstallTransaction.psm1") -Force
foreach($path in @($stateRootFull,$journalFull,$ConfigRoot)){Assert-JCodeMunchNoReparseAncestors $path|Out-Null}
$installLock=$null
if(-not$LockAlreadyHeld){$installLock=Enter-HybridInstallLock -StateRoot $stateRootFull}
$rollbackCandidateRoot=$null
try{

$lifecycle=Join-Path $PSScriptRoot "jcodemunch-lifecycle.ps1"
$statusJson=& pwsh -NoProfile -NonInteractive -File $lifecycle -Command status -StateRoot $stateRootFull -ConfigRoot $ConfigRoot -Json
if($LASTEXITCODE-ne0){throw "Cannot inspect lifecycle before rollback: $statusJson"}
$status=$statusJson|ConvertFrom-Json
if(@($status.leases).Count-gt0){throw "Hybrid rollback is refused while client leases remain; close Codex, Claude and Pi sessions first."}

$journal=Get-Content -LiteralPath $journalFull -Raw|ConvertFrom-Json
$published=@(@($journal.published_runtime_paths)+@($PublishedRuntimePaths)|Where-Object{-not[string]::IsNullOrWhiteSpace([string]$_)}|ForEach-Object{[IO.Path]::GetFullPath([string]$_)}|Sort-Object -Unique)
$binaryTarget=[IO.Path]::GetFullPath([string]$journal.binary_target)
$binaryEntries=@($journal.entries|Where-Object{[string]$_.entry_type-eq"file"-and[string]::Equals([IO.Path]::GetFullPath([string]$_.path),$binaryTarget,[StringComparison]::OrdinalIgnoreCase)})
if($binaryEntries.Count-ne1){throw "Rollback journal must identify exactly one binary target entry."}
$binaryEntry=$binaryEntries[0]
# Stop through the currently installed lifecycle before restoring integration
# files. A first-install rollback may delete that lifecycle; invoking it after
# restore can otherwise orphan the exact owned runtime.
if($null-ne$status.server){
    $stopped=& pwsh -NoProfile -NonInteractive -File $lifecycle -Command stop -StateRoot $stateRootFull -ConfigRoot $ConfigRoot -Json
    if($LASTEXITCODE-ne0){throw "Exact runtime shutdown failed before rollback: $stopped"}
}

# Binary bytes are restored (or removed after a failed first install) through
# the native activation barrier. Direct File.Replace/Remove-Item here could race
# a daemon or frontend that starts between lifecycle shutdown and restoration.
$currentHash=if(Test-Path -LiteralPath $binaryTarget -PathType Leaf){(Get-FileHash -LiteralPath $binaryTarget -Algorithm SHA256).Hash.ToLowerInvariant()}else{$null}
$binaryNeedsRestore=if([bool]$binaryEntry.existed){$currentHash-ne[string]$binaryEntry.content_sha256}else{$null-ne$currentHash}
if($binaryNeedsRestore){
    $activatorPath=if([string]::IsNullOrWhiteSpace($NativeActivator)){$binaryTarget}else{[IO.Path]::GetFullPath($NativeActivator)}
    if(-not(Test-Path -LiteralPath $activatorPath -PathType Leaf)){throw "Native rollback activator is missing: $activatorPath"}
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$activatorPath;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($name in @("HOME","USERPROFILE","CODEX_HOME","CLAUDE_CONFIG_DIR")){
        $value=[string]$journal.activation_environment.$name
        if([string]::IsNullOrWhiteSpace($value)){throw "Rollback journal is missing the native activation environment: $name"}
        $start.Environment[$name]=$value
    }
    $installDir=Split-Path -Parent $binaryTarget
    if([bool]$binaryEntry.existed){
        $backup=[IO.Path]::GetFullPath([string]$binaryEntry.backup_path)
        if(-not(Test-Path -LiteralPath $backup -PathType Leaf)){throw "Rollback binary backup is missing: $backup"}
        if((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash.ToLowerInvariant()-ne[string]$binaryEntry.content_sha256){throw "Rollback binary backup integrity mismatch."}
        $rollbackCandidateParent=Join-Path $stateRootFull "rollback-candidates"
        $rollbackCandidateRoot=Join-Path $rollbackCandidateParent ([Guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $rollbackCandidateRoot -Force|Out-Null
        Set-JCodeMunchOwnerAcl -Path $rollbackCandidateParent
        Set-JCodeMunchOwnerAcl -Path $rollbackCandidateRoot
        $rollbackCandidate=Join-Path $rollbackCandidateRoot "codebase-memory-mcp.exe"
        [IO.File]::Copy($backup,$rollbackCandidate,$false)
        Set-JCodeMunchOwnerAcl -Path $rollbackCandidate -File
        if((Get-FileHash -LiteralPath $rollbackCandidate -Algorithm SHA256).Hash.ToLowerInvariant()-ne[string]$binaryEntry.content_sha256){throw "Private rollback candidate integrity mismatch."}
        foreach($argument in @("install","--yes","--force","--binary-only","--binary-source",$rollbackCandidate,"--dir",$installDir)){[void]$start.ArgumentList.Add([string]$argument)}
    }else{
        foreach($argument in @("uninstall","--yes","--binary-only","--dir",$installDir)){[void]$start.ArgumentList.Add([string]$argument)}
    }
    $process=[Diagnostics.Process]::new();$process.StartInfo=$start
    if(-not$process.Start()){throw "Unable to start native rollback activation."}
    $stdoutTask=$process.StandardOutput.ReadToEndAsync();$stderrTask=$process.StandardError.ReadToEndAsync();$process.WaitForExit()
    $stdout=$stdoutTask.GetAwaiter().GetResult();$stderr=$stderrTask.GetAwaiter().GetResult()
    if($process.ExitCode-ne0){throw "Native rollback activation failed: $(Protect-JCodeMunchDiagnosticText -Text ($stderr+$stdout))"}
}
if([bool]$binaryEntry.existed){
    if(-not(Test-Path -LiteralPath $binaryTarget -PathType Leaf)-or(Get-FileHash -LiteralPath $binaryTarget -Algorithm SHA256).Hash.ToLowerInvariant()-ne[string]$binaryEntry.content_sha256){throw "Native rollback did not restore the recorded binary."}
    if([OperatingSystem]::IsWindows()-and-not[string]::IsNullOrWhiteSpace([string]$binaryEntry.security_descriptor_base64)){
        Set-JCodeMunchSecurityDescriptor -Path $binaryTarget -DescriptorBase64 ([string]$binaryEntry.security_descriptor_base64)
        if(-not(Test-JCodeMunchSecurityDescriptorEquivalent -ExpectedBase64 ([string]$binaryEntry.security_descriptor_base64) -ActualBase64 (Get-JCodeMunchSecurityDescriptor -Path $binaryTarget))){throw "Restored binary security descriptor verification failed."}
    }
}elseif(Test-Path -LiteralPath $binaryTarget){throw "Native rollback did not remove the first-install binary."}

$restore=Restore-HybridInstallJournal -JournalPath $journalFull -SkipPaths @($binaryTarget)

$runtimeRoot=[IO.Path]::GetFullPath((Join-Path $stateRootFull "runtimes"))
$runtimePrefix=$runtimeRoot.TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
foreach($path in $published){
    if([string]::IsNullOrWhiteSpace([string]$path)){continue}
    $full=[IO.Path]::GetFullPath([string]$path)
    if(-not$full.StartsWith($runtimePrefix,[StringComparison]::OrdinalIgnoreCase)){throw "Refusing rollback cleanup outside owned runtime root: $full"}
    Assert-JCodeMunchNoReparseAncestors $runtimeRoot|Out-Null
    Assert-JCodeMunchNoReparseAncestors $full|Out-Null
    if(Test-Path -LiteralPath $full -PathType Container){Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction Stop}
}

$backupPath=Join-Path $stateRootFull "backups.json"
if(Test-Path -LiteralPath $backupPath -PathType Leaf){
    $backups=Get-Content -LiteralPath $backupPath -Raw|ConvertFrom-Json
    $backups.status="rolled_back_by_install_transaction"
    Write-JCodeMunchAtomicText -Path $backupPath -Text ($backups|ConvertTo-Json -Depth 20) -Secret
}

[ordered]@{status="rolled_back";transaction=$restore;runtime_paths_removed=$published.Count}|ConvertTo-Json -Depth 10 -Compress
}finally{
    try{
        if(-not[string]::IsNullOrWhiteSpace($rollbackCandidateRoot)-and(Test-Path -LiteralPath $rollbackCandidateRoot -PathType Container)){
            $candidateParent=[IO.Path]::GetFullPath((Join-Path $stateRootFull "rollback-candidates"))
            $candidatePrefix=$candidateParent.TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
            $candidateFull=[IO.Path]::GetFullPath($rollbackCandidateRoot)
            if(-not$candidateFull.StartsWith($candidatePrefix,[StringComparison]::OrdinalIgnoreCase)){throw "Refusing rollback candidate cleanup outside owned root: $candidateFull"}
            Assert-JCodeMunchNoReparseAncestors $candidateParent|Out-Null
            Assert-JCodeMunchNoReparseAncestors $candidateFull|Out-Null
            Remove-Item -LiteralPath $candidateFull -Recurse -Force -ErrorAction Stop
        }
    }finally{if(-not$LockAlreadyHeld){Exit-HybridInstallLock -Lock $installLock}}
}
