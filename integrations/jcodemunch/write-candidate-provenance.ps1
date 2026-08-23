#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$CandidateBinary,
    [string]$ForkRemote="fork",
    [string]$UpstreamRemote="upstream"
)
Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
Import-Module (Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1") -Force

$source=[IO.Path]::GetFullPath($SourceRoot)
function Invoke-GitText([string[]]$Arguments){
    $output=(& git -C $source @Arguments 2>&1|Out-String).Trim()
    if($LASTEXITCODE-ne0-or[string]::IsNullOrWhiteSpace($output)){throw "git $($Arguments-join' ') failed in $source"}
    return $output
}
function Get-GitStatus {
    $output=(& git -C $source status --porcelain --untracked-files=all 2>&1|Out-String).TrimEnd()
    if($LASTEXITCODE-ne0){throw "Unable to inspect source worktree status."}
    return $output
}

$top=[IO.Path]::GetFullPath((Invoke-GitText @("rev-parse","--show-toplevel")))
if(-not[string]::Equals($top,$source,[StringComparison]::OrdinalIgnoreCase)){throw "SourceRoot must be the git worktree root: $top"}
$dirty=Get-GitStatus
if(-not[string]::IsNullOrWhiteSpace($dirty)){throw "Refusing to build or attest from a dirty or untracked worktree."}

$head=Invoke-GitText @("rev-parse","HEAD")
$sourceTree=Invoke-GitText @("rev-parse","HEAD^{tree}")
$branch=Invoke-GitText @("branch","--show-current")
$forkUrl=Invoke-GitText @("remote","get-url",$ForkRemote)
$upstreamTip=Invoke-GitText @("rev-parse","$UpstreamRemote/main")
$upstreamBase=Invoke-GitText @("merge-base","HEAD","$UpstreamRemote/main")
$buildRelative="build/attested-candidate"
$binaryLeaf=if([OperatingSystem]::IsWindows()){"codebase-memory-mcp.exe"}else{"codebase-memory-mcp"}
$expectedBinary=[IO.Path]::GetFullPath((Join-Path (Join-Path $source $buildRelative) $binaryLeaf))
if(-not[string]::IsNullOrWhiteSpace($CandidateBinary)){
    $requestedBinary=[IO.Path]::GetFullPath($CandidateBinary)
    if(-not[string]::Equals($requestedBinary,$expectedBinary,[StringComparison]::OrdinalIgnoreCase)){throw "CandidateBinary must be the output of the canonical attested build: $expectedBinary"}
}

$bash=(Get-Command bash -ErrorAction Stop).Source
$buildArguments=[Collections.Generic.List[string]]::new()
foreach($argument in @("scripts/build.sh","--with-ui","--version",$head,"BUILD_DIR=$buildRelative")){[void]$buildArguments.Add($argument)}
if([OperatingSystem]::IsWindows()){
    [void]$buildArguments.Add("CC=/clang64/bin/clang.exe")
    [void]$buildArguments.Add("CXX=/clang64/bin/clang++.exe")
    $npmCommand=(Get-Command npm.cmd -ErrorAction Stop).Source
    if($npmCommand-notmatch'^(?<drive>[A-Za-z]):\\(?<tail>.+)$'){throw "Unable to translate npm.cmd into an MSYS path: $npmCommand"}
    $msysNpm="/$($Matches.drive.ToLowerInvariant())/$($Matches.tail.Replace('\','/'))"
    [void]$buildArguments.Add("NPM=`"$msysNpm`"")
}
$buildStarted=[DateTime]::UtcNow
$start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$bash;$start.WorkingDirectory=$source
$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
if([OperatingSystem]::IsWindows()){
    # ProcessStartInfo preserves the Windows PATH order. Without this boundary,
    # MSYS recipes can resolve WSL/System32 bash, find, or ld before the MSYS
    # toolchain and silently emit ELF objects for a Windows target.
    [void]$start.ArgumentList.Add("-c")
    [void]$start.ArgumentList.Add('export PATH="/clang64/bin:/usr/bin:/bin:$PATH"; exec /usr/bin/bash "$@"')
    [void]$start.ArgumentList.Add("cbm-attested-build")
}
foreach($argument in $buildArguments){[void]$start.ArgumentList.Add($argument)}
$process=[Diagnostics.Process]::new();$process.StartInfo=$start
if(-not$process.Start()){throw "Unable to start the canonical production build."}
$stdoutTask=$process.StandardOutput.ReadToEndAsync();$stderrTask=$process.StandardError.ReadToEndAsync();$process.WaitForExit()
$stdout=$stdoutTask.GetAwaiter().GetResult();$stderr=$stderrTask.GetAwaiter().GetResult()
if($process.ExitCode-ne0){throw "Canonical production build failed: $(Protect-JCodeMunchDiagnosticText -Text ($stderr+$stdout))"}
$buildCompleted=[DateTime]::UtcNow

if(-not(Test-Path -LiteralPath $expectedBinary -PathType Leaf)){throw "Canonical build did not publish its expected candidate: $expectedBinary"}
$binaryItem=Get-Item -LiteralPath $expectedBinary -Force
if(($binaryItem.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Canonical candidate must not be a link or reparse point."}

# The UI compiler may update this tracked incremental cache. It was proven
# clean before the build, so restore it only when it is the sole build mutation.
$postBuildDirty=Get-GitStatus
if(-not[string]::IsNullOrWhiteSpace($postBuildDirty)){
    $dirtyLines=@($postBuildDirty-split"`r?`n"|Where-Object{-not[string]::IsNullOrWhiteSpace($_)})
    if($dirtyLines.Count-eq1-and$dirtyLines[0]-match'^.. graph-ui/tsconfig\.tsbuildinfo$'){
        & git -C $source restore --worktree -- graph-ui/tsconfig.tsbuildinfo 2>&1|Out-Null
        if($LASTEXITCODE-ne0){throw "Unable to restore the build-owned TypeScript cache."}
    }
}
$finalDirty=Get-GitStatus
if(-not[string]::IsNullOrWhiteSpace($finalDirty)){throw "Canonical build changed source-controlled or untracked files: $finalDirty"}
$finalHead=Invoke-GitText @("rev-parse","HEAD")
$finalTree=Invoke-GitText @("rev-parse","HEAD^{tree}")
if($finalHead-ne$head-or$finalTree-ne$sourceTree){throw "Source commit or tree changed during the canonical build."}

$integrationRelativeRoot="integrations/jcodemunch";$integrationSourceRoot=[IO.Path]::GetFullPath((Join-Path $source $integrationRelativeRoot))
$trackedIntegrationOutput=(& git -C $source ls-files -- $integrationRelativeRoot 2>&1|Out-String).Trim()
if($LASTEXITCODE-ne0-or[string]::IsNullOrWhiteSpace($trackedIntegrationOutput)){throw "The attested Git tree contains no hybrid integration assets."}
$trackedIntegration=@($trackedIntegrationOutput-split"`r?`n"|Where-Object{-not[string]::IsNullOrWhiteSpace($_)}|Sort-Object -Unique)
$integrationEntries=[Collections.Generic.List[object]]::new();$trackedFull=@{}
foreach($repoRelative in $trackedIntegration){
    $full=[IO.Path]::GetFullPath((Join-Path $source $repoRelative));$prefix=$integrationSourceRoot.TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
    if(-not$full.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)-or-not(Test-Path -LiteralPath $full -PathType Leaf)){throw "Unsafe or missing tracked integration asset: $repoRelative"}
    $item=Get-Item -LiteralPath $full -Force;if(($item.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Tracked integration asset must not be a link or reparse point: $repoRelative"}
    $relative=$full.Substring($integrationSourceRoot.Length).TrimStart([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar).Replace('\','/')
    $trackedFull[$full]=$true;$integrationEntries.Add([ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()})
}
$extraIntegration=@(Get-ChildItem -LiteralPath $integrationSourceRoot -File -Recurse|Where-Object{-not$trackedFull.ContainsKey($_.FullName)})
if($extraIntegration.Count){throw "Ignored or untracked files exist inside the integration source: $($extraIntegration.FullName-join', ')"}

$versionOutput=(& $expectedBinary --version 2>&1|Out-String).Trim()
if($LASTEXITCODE-ne0-or$versionOutput-notmatch[regex]::Escape($head)){throw "Canonical candidate version is not stamped with source commit $head."}
$candidateHash=(Get-FileHash -LiteralPath $expectedBinary -Algorithm SHA256).Hash.ToLowerInvariant()
$transcriptBytes=[Text.Encoding]::UTF8.GetBytes($stdout+"`n--- stderr ---`n"+$stderr)
$transcriptHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($transcriptBytes)).ToLowerInvariant()
$document=[ordered]@{
    schema_version=4
    product="codebase-memory-mcp"
    attestation_type="canonical-clean-build"
    source_root=$source
    source_commit=$head
    source_tree=$sourceTree
    source_branch=$branch
    tracked_tree_clean=$true
    worktree_clean=$true
    fork=[ordered]@{remote=$ForkRemote;url=$forkUrl}
    upstream=[ordered]@{remote=$UpstreamRemote;tip=$upstreamTip;merge_base=$upstreamBase}
    integration_assets=[ordered]@{root=$integrationRelativeRoot;files=@($integrationEntries);file_count=$integrationEntries.Count}
    build=[ordered]@{
        mode="canonical-clean-build"
        entrypoint="scripts/build.sh"
        arguments=@($buildArguments)
        build_directory=$buildRelative
        source_commit_before=$head
        source_commit_after=$finalHead
        source_tree_before=$sourceTree
        source_tree_after=$finalTree
        post_build_worktree_clean=$true
        transcript_sha256=$transcriptHash
        started_at=$buildStarted.ToString("o")
        completed_at=$buildCompleted.ToString("o")
    }
    candidate=[ordered]@{path=$expectedBinary;sha256=$candidateHash;version=$versionOutput;source_commit_stamp=$head}
    generated_at=[DateTime]::UtcNow.ToString("o")
}
Write-JCodeMunchAtomicText -Path $OutputPath -Text ($document|ConvertTo-Json -Depth 15) -Secret
$document|ConvertTo-Json -Depth 15 -Compress
