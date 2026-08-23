#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CandidateBinary,
    [Parameter(Mandatory=$true)][string]$CandidateProvenance,
    [string]$InstallDir,
    [string]$StateRoot,
    [string]$ClientHome,
    [string]$CodexConfig,
    [string]$ClaudeConfig,
    [string]$ClaudeSettingsConfig,
    [string]$PiConfig,
    [string]$PiSettingsConfig
)

Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
Import-Module (Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1") -Force
Import-Module (Join-Path $PSScriptRoot "HybridInstallTransaction.psm1") -Force

$candidate=[IO.Path]::GetFullPath($CandidateBinary)
$activationCandidate=$candidate
$provenancePath=[IO.Path]::GetFullPath($CandidateProvenance)
if(-not(Test-Path -LiteralPath $candidate -PathType Leaf)){throw "Candidate binary is missing: $candidate"}
if(-not(Test-Path -LiteralPath $provenancePath -PathType Leaf)){throw "Candidate provenance is missing: $provenancePath"}
$provenance=Get-Content -LiteralPath $provenancePath -Raw|ConvertFrom-Json
if([int]$provenance.schema_version-ne4-or[string]$provenance.product-ne"codebase-memory-mcp"-or[string]$provenance.attestation_type-ne"canonical-clean-build"-or[string]$provenance.build.mode-ne"canonical-clean-build"-or-not[bool]$provenance.worktree_clean-or-not[bool]$provenance.build.post_build_worktree_clean){throw "Candidate provenance is invalid."}
if([string]$provenance.source_commit-ne[string]$provenance.build.source_commit_before-or[string]$provenance.source_commit-ne[string]$provenance.build.source_commit_after-or[string]$provenance.source_tree-ne[string]$provenance.build.source_tree_before-or[string]$provenance.source_tree-ne[string]$provenance.build.source_tree_after){throw "Candidate provenance source identity is inconsistent."}
if(-not[string]::Equals([IO.Path]::GetFullPath([string]$provenance.candidate.path),$candidate,[StringComparison]::OrdinalIgnoreCase)){throw "Candidate path does not match its canonical build provenance."}
if((Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()-ne[string]$provenance.candidate.sha256){throw "Candidate binary no longer matches its provenance."}
if([string]::IsNullOrWhiteSpace([string]$provenance.build.transcript_sha256)){throw "Candidate build transcript digest is missing."}
$attestedSource=[IO.Path]::GetFullPath([string]$provenance.source_root)
function Invoke-AttestedGitText([string[]]$Arguments){$output=(& git -C $attestedSource @Arguments 2>&1|Out-String).Trim();if($LASTEXITCODE-ne0){throw "Unable to revalidate attested source identity."};return $output}
if((Invoke-AttestedGitText @("rev-parse","HEAD"))-ne[string]$provenance.source_commit-or(Invoke-AttestedGitText @("rev-parse","HEAD^{tree}"))-ne[string]$provenance.source_tree-or(Invoke-AttestedGitText @("remote","get-url",[string]$provenance.fork.remote))-ne[string]$provenance.fork.url){throw "Attested source identity changed before installation."}
if(-not[string]::IsNullOrWhiteSpace((Invoke-AttestedGitText @("status","--porcelain","--untracked-files=all")))){throw "Attested source worktree changed before installation."}
$attestedIntegrationRoot=[IO.Path]::GetFullPath((Join-Path $attestedSource ([string]$provenance.integration_assets.root)))
if(-not[string]::Equals($attestedIntegrationRoot,[IO.Path]::GetFullPath($PSScriptRoot),[StringComparison]::OrdinalIgnoreCase)){throw "Installer must execute from the attested integration root."}
$integrationAssetRelatives=[Collections.Generic.List[string]]::new();$expectedSourceAssets=@{}
foreach($entry in @($provenance.integration_assets.files)){
    $relative=[string]$entry.path
    if([string]::IsNullOrWhiteSpace($relative)-or[IO.Path]::IsPathRooted($relative)-or$relative-match'(^|/)\.\.(/|$)'-or$expectedSourceAssets.ContainsKey($relative)){throw "Candidate provenance contains an unsafe or duplicate integration asset path."}
    $sourcePath=[IO.Path]::GetFullPath((Join-Path $attestedIntegrationRoot ($relative-replace'/',[IO.Path]::DirectorySeparatorChar)));$sourcePrefix=$attestedIntegrationRoot.TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
    if(-not$sourcePath.StartsWith($sourcePrefix,[StringComparison]::OrdinalIgnoreCase)-or-not(Test-Path -LiteralPath $sourcePath -PathType Leaf)){throw "Attested integration asset is missing: $relative"}
    Assert-JCodeMunchNoReparseAncestors $sourcePath|Out-Null
    if((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()-ne[string]$entry.sha256){throw "Attested integration asset hash mismatch: $relative"}
    $expectedSourceAssets[$relative]=$true;$integrationAssetRelatives.Add($relative)
}
if($integrationAssetRelatives.Count-ne[int]$provenance.integration_assets.file_count-or$integrationAssetRelatives.Count-lt1){throw "Candidate provenance integration asset count is invalid."}
$actualSourceAssets=@(Get-ChildItem -LiteralPath $attestedIntegrationRoot -File -Recurse|ForEach-Object{$_.FullName.Substring($attestedIntegrationRoot.Length).TrimStart([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar).Replace('\','/')})
if(@($actualSourceAssets|Where-Object{-not$expectedSourceAssets.ContainsKey($_)}).Count-ne0-or$actualSourceAssets.Count-ne$integrationAssetRelatives.Count){throw "Integration source contains ignored, untracked, or unexpected assets."}

if([string]::IsNullOrWhiteSpace($ClientHome)){$ClientHome=[Environment]::GetFolderPath("UserProfile")}
$clientHomeFull=[IO.Path]::GetFullPath($ClientHome)
if([string]::IsNullOrWhiteSpace($InstallDir)){$InstallDir=Join-Path $clientHomeFull ".local\bin"}
if([string]::IsNullOrWhiteSpace($StateRoot)){$StateRoot=Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "CodebaseMemoryMcp\companions\jcodemunch"}
if([string]::IsNullOrWhiteSpace($CodexConfig)){$CodexConfig=Join-Path $clientHomeFull ".codex\config.toml"}
if([string]::IsNullOrWhiteSpace($ClaudeConfig)){$ClaudeConfig=Join-Path $clientHomeFull ".claude.json"}
if([string]::IsNullOrWhiteSpace($ClaudeSettingsConfig)){$ClaudeSettingsConfig=Join-Path $clientHomeFull ".claude\settings.json"}
if([string]::IsNullOrWhiteSpace($PiConfig)){$PiConfig=Join-Path $clientHomeFull ".pi\agent\mcp.json"}
if([string]::IsNullOrWhiteSpace($PiSettingsConfig)){$PiSettingsConfig=Join-Path $clientHomeFull ".pi\agent\settings.json"}

function Assert-ExpectedPath([string]$Name,[string]$Actual,[string]$Expected){
    $actualFull=[IO.Path]::GetFullPath($Actual);$expectedFull=[IO.Path]::GetFullPath($Expected)
    if(-not[string]::Equals($actualFull,$expectedFull,[StringComparison]::OrdinalIgnoreCase)){throw "$Name must be rooted coherently under ClientHome. Expected $expectedFull, got $actualFull"}
    return $actualFull
}
$CodexConfig=Assert-ExpectedPath "CodexConfig" $CodexConfig (Join-Path $clientHomeFull ".codex\config.toml")
$ClaudeConfig=Assert-ExpectedPath "ClaudeConfig" $ClaudeConfig (Join-Path $clientHomeFull ".claude.json")
$ClaudeSettingsConfig=Assert-ExpectedPath "ClaudeSettingsConfig" $ClaudeSettingsConfig (Join-Path $clientHomeFull ".claude\settings.json")
$PiConfig=Assert-ExpectedPath "PiConfig" $PiConfig (Join-Path $clientHomeFull ".pi\agent\mcp.json")
$PiSettingsConfig=Assert-ExpectedPath "PiSettingsConfig" $PiSettingsConfig (Join-Path $clientHomeFull ".pi\agent\settings.json")

$stateRootFull=[IO.Path]::GetFullPath($StateRoot)
$installDirFull=[IO.Path]::GetFullPath($InstallDir)
foreach($path in @($clientHomeFull,$stateRootFull,$installDirFull)){Assert-JCodeMunchNoReparseAncestors $path|Out-Null}
$integrationRoot=Join-Path $stateRootFull "integration"
$runtimeRoot=Join-Path $stateRootFull "runtimes"
$receiptPath=Join-Path $stateRootFull "install-receipt.json"
$journalPath=Join-Path $stateRootFull "install-transaction.json"
$installedBinary=Join-Path $installDirFull "codebase-memory-mcp.exe"

function Invoke-NativeInstallCommand([string[]]$Arguments){
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$activationCandidate;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($argument in $Arguments){[void]$start.ArgumentList.Add($argument)}
    $start.Environment["HOME"]=$clientHomeFull
    $start.Environment["USERPROFILE"]=$clientHomeFull
    $start.Environment["CODEX_HOME"]=Split-Path -Parent $CodexConfig
    $start.Environment["CLAUDE_CONFIG_DIR"]=Split-Path -Parent $ClaudeSettingsConfig
    $process=[Diagnostics.Process]::new();$process.StartInfo=$start
    if(-not$process.Start()){throw "Unable to start native fork installer."}
    $stdoutTask=$process.StandardOutput.ReadToEndAsync();$stderrTask=$process.StandardError.ReadToEndAsync();$process.WaitForExit()
    $stdout=$stdoutTask.GetAwaiter().GetResult();$stderr=$stderrTask.GetAwaiter().GetResult()
    return [pscustomobject]@{exit_code=$process.ExitCode;stdout=$stdout;stderr=$stderr}
}

function Test-CbmMcpHandshake([string]$Binary){
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$Binary;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardInput=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$start
    if(-not$process.Start()){throw "Unable to start installed MCP binary."}
    try{
        $initialize='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"hybrid-installer","version":"1"}}}'
        $process.StandardInput.WriteLine($initialize);$process.StandardInput.Flush();$read=$process.StandardOutput.ReadLineAsync()
        if(-not$read.Wait([TimeSpan]::FromSeconds(20))){throw "Installed MCP initialize timed out."}
        $response=$read.Result|ConvertFrom-Json -ErrorAction Stop
        if($null-eq$response.result-or[string]$response.result.serverInfo.name-ne"codebase-memory-mcp"){throw "Installed MCP initialize response is invalid."}
    }finally{try{$process.StandardInput.Close()}catch{};if(-not$process.WaitForExit(10000)){try{$process.Kill($true)}catch{}}}
}

New-Item -ItemType Directory -Path $stateRootFull -Force|Out-Null
Assert-JCodeMunchNoReparseAncestors $stateRootFull|Out-Null
Set-JCodeMunchOwnerAcl -Path $stateRootFull
$installLock=Enter-HybridInstallLock -StateRoot $stateRootFull
$candidateStageRoot=$null
try{
$candidateStageParent=Join-Path $stateRootFull "candidate-staging"
$candidateStageRoot=Join-Path $candidateStageParent ([Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $candidateStageRoot -Force|Out-Null
Set-JCodeMunchOwnerAcl -Path $candidateStageParent
Set-JCodeMunchOwnerAcl -Path $candidateStageRoot
$activationCandidate=Join-Path $candidateStageRoot "codebase-memory-mcp.exe"
[IO.File]::Copy($candidate,$activationCandidate,$false)
Set-JCodeMunchOwnerAcl -Path $activationCandidate -File
if((Get-FileHash -LiteralPath $activationCandidate -Algorithm SHA256).Hash.ToLowerInvariant()-ne[string]$provenance.candidate.sha256){throw "Private activation candidate hash mismatch."}
$planResult=Invoke-NativeInstallCommand @("install","--plan","--skip-binary","--dir",$installDirFull,"--clients=claude,codex,pi")
if($planResult.exit_code-ne0){throw "Native install plan failed: $($planResult.stderr)"}
try{$plan=$planResult.stdout|ConvertFrom-Json -ErrorAction Stop}catch{throw "Native install plan returned invalid JSON: $($_.Exception.Message)"}
if([string]$plan.type-ne"agent.install.plan.v1"-or[bool]$plan.writes_started){throw "Native install plan contract is invalid."}

$targets=[Collections.Generic.List[string]]::new()
foreach($property in @("config_files_planned","instruction_files_planned","skill_files_planned","agent_files_planned","prompt_files_planned")){
    foreach($path in @($plan.$property)){if(-not[string]::IsNullOrWhiteSpace([string]$path)){$targets.Add([string]$path)}}
}
foreach($hook in @($plan.hooks_planned)){if(-not[string]::IsNullOrWhiteSpace([string]$hook.path)){$targets.Add([string]$hook.path)}}
foreach($path in @($plan.cleanup_files_planned)){if(-not[string]::IsNullOrWhiteSpace([string]$path)){$targets.Add([string]$path)}}
$directoryTargets=@($plan.cleanup_directories_planned|Where-Object{-not[string]::IsNullOrWhiteSpace([string]$_)})
foreach($path in @($installedBinary,$CodexConfig,$ClaudeConfig,$ClaudeSettingsConfig,$PiConfig,$PiSettingsConfig,$receiptPath,(Join-Path $clientHomeFull ".claude\.mcp.json"))){$targets.Add($path)}
foreach($relative in $integrationAssetRelatives){$targets.Add((Join-Path $integrationRoot ($relative-replace'/',[IO.Path]::DirectorySeparatorChar)))}
if(Test-Path -LiteralPath $integrationRoot -PathType Container){
    Assert-JCodeMunchNoReparseAncestors $integrationRoot|Out-Null
    foreach($item in @(Get-ChildItem -LiteralPath $integrationRoot -Force -Recurse)){if(($item.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Installed integration contains a link or reparse point: $($item.FullName)"};if(-not$item.PSIsContainer){$targets.Add($item.FullName)}}
}
$targets.Add((Join-Path $integrationRoot "integration-manifest.json"))

$activationEnvironment=@{HOME=$clientHomeFull;USERPROFILE=$clientHomeFull;CODEX_HOME=(Split-Path -Parent $CodexConfig);CLAUDE_CONFIG_DIR=(Split-Path -Parent $ClaudeSettingsConfig)}
$journal=New-HybridInstallJournal -StateRoot $stateRootFull -JournalPath $journalPath -Paths @($targets) -DirectoryPaths $directoryTargets -BinaryTarget $installedBinary -ActivationEnvironment $activationEnvironment
$publishedRuntimePaths=@()
try{
    $stageJson=& (Join-Path $PSScriptRoot "stage-runtime.ps1") -DestinationRoot $runtimeRoot -ReceiptPath $receiptPath
    if($LASTEXITCODE-ne0){throw "Companion staging failed."}
    $stageResult=$stageJson|ConvertFrom-Json
    $publishedRuntimePaths=@($stageResult.published_paths)
    [void](Set-HybridInstallJournalStatus -JournalPath $journalPath -Status prepared -PublishedRuntimePaths $publishedRuntimePaths)

    $binaryInstall=Invoke-NativeInstallCommand @("install","--yes","--force","--binary-only","--dir",$installDirFull)
    if($binaryInstall.exit_code-ne0){throw "Native binary activation failed: $($binaryInstall.stderr)$($binaryInstall.stdout)"}
    $native=Invoke-NativeInstallCommand @("install","--yes","--force","--skip-binary","--dir",$installDirFull,"--clients=claude,codex,pi")
    if($native.exit_code-ne0){throw "Native fork installation failed: $($native.stderr)$($native.stdout)"}

    $configJson=& (Join-Path $PSScriptRoot "jcodemunch-lifecycle.ps1") -Command configure -StateRoot $stateRootFull -ConfigRoot $stateRootFull -ApplyClientConfigs -CodexConfig $CodexConfig -ClaudeConfig $ClaudeConfig -ClaudeSettingsConfig $ClaudeSettingsConfig -PiConfig $PiConfig -PiSettingsConfig $PiSettingsConfig -IntegrationRoot $integrationRoot -SourceAssetPaths @($integrationAssetRelatives) -Json
    if($LASTEXITCODE-ne0){throw "Hybrid client configuration failed: $configJson"}
    $configPayload=$configJson|ConvertFrom-Json
    $integrationManifestJson=& (Join-Path $integrationRoot "write-integration-manifest.ps1") -IntegrationRoot $integrationRoot
    if($LASTEXITCODE-ne0){throw "Integration asset manifest publication failed."}
    $integrationManifest=$integrationManifestJson|ConvertFrom-Json

    if(-not(Test-Path -LiteralPath $installedBinary -PathType Leaf)){throw "Verified binary publication is missing."}
    Test-CbmMcpHandshake $installedBinary
    $runtimeVerifyArguments=@{RuntimeRoot=$runtimeRoot;IntegrationRoot=$integrationRoot;ExpectedIntegrationManifestSha256=[string]$integrationManifest.manifest_sha256}
    if([bool]$stageResult.reused){$runtimeVerifyArguments.ReceiptPath=$receiptPath}
    $verifiedRuntime=& (Join-Path $integrationRoot "verify-runtime.ps1") @runtimeVerifyArguments
    if($LASTEXITCODE-ne0){throw "Installed companion verification failed."}
    $receipt=& (Join-Path $integrationRoot "write-install-receipt.ps1") -ReceiptPath $receiptPath -CandidateProvenance $provenancePath -CandidateBinary $candidate -InstalledBinary $installedBinary -SnapshotTransactionId ([string]$journal.transaction_id) -RuntimeVerification $verifiedRuntime -ClientConfigPaths @($CodexConfig,$ClaudeConfig,$ClaudeSettingsConfig,$PiConfig,$PiSettingsConfig)
    if($LASTEXITCODE-ne0){throw "Installation receipt publication failed."}
    [void](Set-HybridInstallJournalStatus -JournalPath $journalPath -Status applied -PublishedRuntimePaths $publishedRuntimePaths)
    $receipt
}catch{
    $cause=$_.Exception.Message
    try{$rollback=& (Join-Path $PSScriptRoot "rollback-hybrid.ps1") -StateRoot $stateRootFull -ConfigRoot $stateRootFull -JournalPath $journalPath -NativeActivator $activationCandidate -PublishedRuntimePaths $publishedRuntimePaths -LockAlreadyHeld}
    catch{throw "$cause Rollback also failed: $($_.Exception.Message)"}
    throw "$cause Installation was rolled back and verified."
}
}finally{
    try{
        if(-not[string]::IsNullOrWhiteSpace($candidateStageRoot)-and(Test-Path -LiteralPath $candidateStageRoot -PathType Container)){
            $candidatePrefix=([IO.Path]::GetFullPath((Join-Path $stateRootFull "candidate-staging"))).TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
            $candidateStageFull=[IO.Path]::GetFullPath($candidateStageRoot)
            if(-not$candidateStageFull.StartsWith($candidatePrefix,[StringComparison]::OrdinalIgnoreCase)){throw "Refusing candidate staging cleanup outside owned root: $candidateStageFull"}
            Remove-Item -LiteralPath $candidateStageFull -Recurse -Force -ErrorAction Stop
        }
    }finally{
        Exit-HybridInstallLock -Lock $installLock
    }
}
