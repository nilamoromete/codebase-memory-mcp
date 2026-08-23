#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ReceiptPath,
    [Parameter(Mandatory=$true)][string]$CandidateProvenance,
    [Parameter(Mandatory=$true)][string]$CandidateBinary,
    [Parameter(Mandatory=$true)][string]$InstalledBinary,
    [Parameter(Mandatory=$true)][string]$SnapshotTransactionId,
    [Parameter(Mandatory=$true)][string]$RuntimeVerification,
    [string[]]$ClientConfigPaths=@()
)

Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
Import-Module (Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1") -Force
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot "versions.json") -Raw|ConvertFrom-Json
$provenance=Get-Content -LiteralPath $CandidateProvenance -Raw|ConvertFrom-Json
if([int]$provenance.schema_version-ne4-or[string]$provenance.product-ne"codebase-memory-mcp"-or[string]$provenance.attestation_type-ne"canonical-clean-build"-or-not[bool]$provenance.worktree_clean-or-not[bool]$provenance.build.post_build_worktree_clean){throw "Candidate provenance is invalid or unclean."}
foreach($path in @($CandidateBinary,$InstalledBinary)){if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw "Required binary is missing: $path"}}
$candidateHash=(Get-FileHash -LiteralPath $CandidateBinary -Algorithm SHA256).Hash.ToLowerInvariant()
$installedHash=(Get-FileHash -LiteralPath $InstalledBinary -Algorithm SHA256).Hash.ToLowerInvariant()
if($candidateHash-ne$installedHash-or$candidateHash-ne[string]$provenance.candidate.sha256){throw "Installed binary hash does not match candidate provenance."}
$versionOutput=(& $InstalledBinary --version 2>&1|Out-String).Trim()
if($LASTEXITCODE-ne0-or$versionOutput-notmatch'^codebase-memory-mcp\s+\S+') {throw "Installed binary version verification failed."}
$runtimeVerificationDocument=$RuntimeVerification|ConvertFrom-Json
if(-not[bool]$runtimeVerificationDocument.verified){throw "Runtime verification receipt is invalid."}
$configReceipts=[Collections.Generic.List[object]]::new()
foreach($path in @($ClientConfigPaths|Sort-Object -Unique)){
    $full=[IO.Path]::GetFullPath($path)
    $hash=if(Test-Path -LiteralPath $full -PathType Leaf){(Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()}else{$null}
    $configReceipts.Add([ordered]@{path=$full;sha256=$hash})
}
$receipt=[ordered]@{
    schema_version=2
    product="codebase-memory-mcp-hybrid"
    status="installed"
    source=[ordered]@{
        root=[string]$provenance.source_root
        branch=[string]$provenance.source_branch
        commit=[string]$provenance.source_commit
        fork=[ordered]@{remote=[string]$provenance.fork.remote;url=[string]$provenance.fork.url}
        upstream=[ordered]@{remote=[string]$provenance.upstream.remote;tip=[string]$provenance.upstream.tip;merge_base=[string]$provenance.upstream.merge_base}
    }
    binary=[ordered]@{path=[IO.Path]::GetFullPath($InstalledBinary);sha256=$installedHash;version=$versionOutput}
    jcodemunch=[ordered]@{version=$manifest.jcodemunch.version;sha256=$manifest.jcodemunch.sha256;transport=$manifest.jcodemunch.transport;bind_host=$manifest.jcodemunch.bind_host}
    stdio_proxy=[ordered]@{package=$manifest.stdio_proxy.package;version=$manifest.stdio_proxy.version;integrity=$manifest.stdio_proxy.integrity;transport=$manifest.stdio_proxy.transport}
    pi_adapter=[ordered]@{package=$manifest.pi_adapter.package;version=$manifest.pi_adapter.version;integrity=$manifest.pi_adapter.integrity}
    runtime_integrity=[ordered]@{
        jcodemunch_manifest_sha256=[string]$runtimeVerificationDocument.jcodemunch.manifest_sha256
        stdio_proxy_manifest_sha256=[string]$runtimeVerificationDocument.stdio_proxy.manifest_sha256
        pi_adapter_manifest_sha256=[string]$runtimeVerificationDocument.pi_adapter.manifest_sha256
    }
    integration_assets=[ordered]@{schema_version=1;manifest_sha256=[string]$runtimeVerificationDocument.integration_assets.manifest_sha256;file_count=[int]$runtimeVerificationDocument.integration_assets.file_count}
    snapshot_transaction_id=$SnapshotTransactionId
    client_configs=@($configReceipts)
    installed_at=[DateTime]::UtcNow.ToString("o")
    architecture=[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
}
Write-JCodeMunchAtomicText -Path $ReceiptPath -Text ($receipt|ConvertTo-Json -Depth 15) -Secret
$receipt|ConvertTo-Json -Depth 15 -Compress
