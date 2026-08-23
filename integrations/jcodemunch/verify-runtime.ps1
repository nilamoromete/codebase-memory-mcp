#Requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$RuntimeRoot,[string]$ReceiptPath,[string]$IntegrationRoot,[string]$ExpectedIntegrationManifestSha256)
Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"

function Assert-NoReparseAncestors([string]$Path){
    $cursor=[IO.Path]::GetFullPath($Path)
    while(-not[string]::IsNullOrWhiteSpace($cursor)){
        if(Test-Path -LiteralPath $cursor){$item=Get-Item -LiteralPath $cursor -Force;if(($item.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Path ancestry contains a link or reparse point: $cursor"}}
        $parent=[IO.Directory]::GetParent($cursor);if($null-eq$parent-or[string]::Equals($parent.FullName,$cursor,[StringComparison]::OrdinalIgnoreCase)){break};$cursor=$parent.FullName
    }
}

function Assert-NoReparsePoint([string]$RootPath){
    $full=[IO.Path]::GetFullPath($RootPath)
    Assert-NoReparseAncestors $full
    $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($full)
    while($pending.Count-gt0){
        $directory=$pending.Pop();$item=Get-Item -LiteralPath $directory -Force
        if(($item.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Integrity scope contains a link or reparse point: $directory"}
        foreach($child in @(Get-ChildItem -LiteralPath $directory -Force)){
            if(($child.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Integrity scope contains a link or reparse point: $($child.FullName)"}
            if($child.PSIsContainer){$pending.Push($child.FullName)}
        }
    }
}

function Assert-RegularNonReparseFile([string]$Path,[string]$Description){
    if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){throw "$Description is missing: $Path"}
    $item=Get-Item -LiteralPath $Path -Force
    if(($item.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "$Description must not be a link or reparse point: $Path"}
}

function Test-ComponentManifest([string]$Base){
    $path=Join-Path $Base "installation-manifest.json"
    Assert-RegularNonReparseFile $path "Installed-code manifest"
    $document=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    if([int]$document.schema_version-ne1-or@($document.files).Count-lt1){throw "Installed-code manifest is invalid: $path"}
    $expected=@{};foreach($entry in @($document.files)){if([string]::IsNullOrWhiteSpace([string]$entry.path)-or$expected.ContainsKey([string]$entry.path)-or[string]$entry.path-match'(^|/)\.\.(/|$)'-or[string]$entry.path-match'(?i)\.pyc$'){throw "Installed-code manifest contains an unsafe or duplicate path."};$expected[[string]$entry.path]=[string]$entry.sha256}
    $actual=@{}
    foreach($scope in @($document.scopes)){
        if([IO.Path]::IsPathRooted([string]$scope)-or[string]$scope-match'(^|/)\.\.(/|$)'){throw "Installed-code manifest contains an unsafe scope."}
        $scopeRoot=[IO.Path]::GetFullPath((Join-Path $Base ([string]$scope)))
        $basePrefix=[IO.Path]::GetFullPath($Base).TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
        if(-not$scopeRoot.StartsWith($basePrefix,[StringComparison]::OrdinalIgnoreCase)-or-not(Test-Path -LiteralPath $scopeRoot -PathType Container)){throw "Installed-code scope is missing or escaped: $scopeRoot"}
        Assert-NoReparsePoint $scopeRoot
        foreach($file in @(Get-ChildItem -LiteralPath $scopeRoot -File -Recurse)){
            if($file.Extension-eq'.pyc'){throw "Executable Python bytecode is forbidden in the installed runtime: $($file.FullName)"}
            $relative=$file.FullName.Substring($Base.Length).TrimStart([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar).Replace('\','/')
            $actual[$relative]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    if($actual.Count-ne$expected.Count){throw "Installed-code file set changed for $($document.component)."}
    foreach($key in $expected.Keys){if(-not$actual.ContainsKey($key)-or$actual[$key]-ne$expected[$key]){throw "Installed-code integrity mismatch: $key"}}
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-IntegrationManifest([string]$Base){
    $integration=[IO.Path]::GetFullPath($Base);$path=Join-Path $integration "integration-manifest.json"
    Assert-RegularNonReparseFile $path "Integration asset manifest"
    $document=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    if([int]$document.schema_version-ne1-or[string]$document.product-ne"codebase-memory-mcp-hybrid-integration"-or@($document.files).Count-lt1){throw "Integration asset manifest is invalid."}
    Assert-NoReparsePoint $integration
    $expected=@{};foreach($entry in @($document.files)){if([string]::IsNullOrWhiteSpace([string]$entry.path)-or$expected.ContainsKey([string]$entry.path)-or[string]$entry.path-match'(^|/)\.\.(/|$)'){throw "Integration asset manifest contains an unsafe or duplicate path."};$expected[[string]$entry.path]=[string]$entry.sha256}
    $actual=@{};foreach($file in @(Get-ChildItem -LiteralPath $integration -File -Recurse|Where-Object{-not[string]::Equals($_.FullName,$path,[StringComparison]::OrdinalIgnoreCase)})){$relative=$file.FullName.Substring($integration.Length).TrimStart([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar).Replace('\','/');$actual[$relative]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    if($actual.Count-ne$expected.Count){throw "Integration asset file set changed."}
    foreach($key in $expected.Keys){if(-not$actual.ContainsKey($key)-or$actual[$key]-ne$expected[$key]){throw "Integration asset integrity mismatch: $key"}}
    return [pscustomobject]@{hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();file_count=$expected.Count}
}

# Validate external anchors before using any installed integration asset to
# locate or execute runtime code. Fresh installs provide the expected manifest
# hash directly; established installs use the owner-private receipt.
$receipt=$null
if(-not[string]::IsNullOrWhiteSpace($ReceiptPath)){
    if(-not(Test-Path -LiteralPath $ReceiptPath -PathType Leaf)){throw "Installed receipt is missing: $ReceiptPath"}
    Assert-RegularNonReparseFile $ReceiptPath "Installed receipt"
    $receipt=Get-Content -LiteralPath $ReceiptPath -Raw|ConvertFrom-Json
    if([int]$receipt.schema_version-ne2-or[string]$receipt.product-ne"codebase-memory-mcp-hybrid"-or[string]$receipt.status-ne"installed"){throw "Installed receipt is invalid."}
}
$integrationValidation=if([string]::IsNullOrWhiteSpace($IntegrationRoot)){$null}else{Test-IntegrationManifest $IntegrationRoot}
$integrationManifestHash=if($null-eq$integrationValidation){$null}else{[string]$integrationValidation.hash}
if($null-ne$integrationManifestHash){
    $integrationExpected=if(-not[string]::IsNullOrWhiteSpace($ExpectedIntegrationManifestSha256)){$ExpectedIntegrationManifestSha256}elseif($null-ne$receipt){[string]$receipt.integration_assets.manifest_sha256}else{$null}
    if([string]::IsNullOrWhiteSpace($integrationExpected)){throw "Integration verification requires an expected manifest hash or installed receipt."}
    if($integrationManifestHash-ne$integrationExpected.ToLowerInvariant()){throw "Installed receipt integrity mismatch for integration assets."}
}

$versionsPath=Join-Path $PSScriptRoot "versions.json"
Assert-RegularNonReparseFile $versionsPath "Pinned version manifest"
$manifest=Get-Content -Raw -LiteralPath $versionsPath|ConvertFrom-Json
$runtime=$manifest.jcodemunch;$proxy=$manifest.stdio_proxy;$pi=$manifest.pi_adapter;$root=[IO.Path]::GetFullPath($RuntimeRoot)
$runtimeVersionRoot=Join-Path (Join-Path $root ([string]$runtime.package)) ([string]$runtime.version)
$proxyVersionRoot=Join-Path (Join-Path $root ([string]$proxy.package)) ([string]$proxy.version)
$piVersionRoot=Join-Path (Join-Path $root ([string]$pi.package)) ([string]$pi.version)

# Verify every byte and receipt anchor before executing the staged interpreter.
$wheel=Join-Path $runtimeVersionRoot ([string]$runtime.artifact);$runtimePython=Join-Path $runtimeVersionRoot "venv\Scripts\python.exe"
Assert-RegularNonReparseFile $wheel "Pinned jCodeMunch artifact"
if((Get-FileHash -LiteralPath $wheel -Algorithm SHA256).Hash.ToLowerInvariant()-ne([string]$runtime.sha256).ToLowerInvariant()){throw "Pinned jCodeMunch artifact integrity check failed."}
Assert-RegularNonReparseFile $runtimePython "Pinned jCodeMunch Python runtime"
$runtimeManifestHash=Test-ComponentManifest $runtimeVersionRoot

$proxyEntrypoint=Join-Path $proxyVersionRoot (([string]$proxy.entrypoint)-replace'/',[IO.Path]::DirectorySeparatorChar)
Assert-RegularNonReparseFile $proxyEntrypoint "Pinned mcp-remote entrypoint"
$proxyPackagePath=Join-Path $proxyVersionRoot "node_modules\mcp-remote\package.json";$proxyLockPath=Join-Path $proxyVersionRoot "package-lock.json"
Assert-RegularNonReparseFile $proxyPackagePath "Installed mcp-remote package manifest";Assert-RegularNonReparseFile $proxyLockPath "Installed mcp-remote lockfile"
$installedProxy=Get-Content -LiteralPath $proxyPackagePath -Raw|ConvertFrom-Json
$lock=Get-Content -LiteralPath $proxyLockPath -Raw|ConvertFrom-Json -AsHashtable;$locked=$lock.packages["node_modules/mcp-remote"]
if([string]$installedProxy.version-ne[string]$proxy.version-or[string]$locked.integrity-ne[string]$proxy.integrity){throw "mcp-remote version or lockfile integrity mismatch."}
$proxyManifestHash=Test-ComponentManifest $proxyVersionRoot

Assert-NoReparsePoint $piVersionRoot
$piManifestPath=Join-Path $piVersionRoot "installation-manifest.json";Assert-RegularNonReparseFile $piManifestPath "Pi extension manifest"
$piInstallManifest=Get-Content -LiteralPath $piManifestPath -Raw|ConvertFrom-Json
if([string]$piInstallManifest.integrity-ne[string]$pi.integrity-or[string]$piInstallManifest.version-ne[string]$pi.version){throw "Pi extension manifest mismatch."}
$piArtifact=Join-Path $piVersionRoot ([string]$piInstallManifest.artifact)
Assert-RegularNonReparseFile $piArtifact "Pinned Pi extension artifact"
$piHash=[Convert]::ToBase64String([Security.Cryptography.SHA512]::HashData([IO.File]::ReadAllBytes($piArtifact)))
if("sha512-$piHash"-ne[string]$pi.integrity){throw "Pinned Pi extension integrity mismatch."}
$piManifestHash=(Get-FileHash -LiteralPath $piManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()

if($null-ne$receipt){
    foreach($pair in @(
        @{name="jcodemunch";actual=$runtimeManifestHash;expected=[string]$receipt.runtime_integrity.jcodemunch_manifest_sha256},
        @{name="stdio_proxy";actual=$proxyManifestHash;expected=[string]$receipt.runtime_integrity.stdio_proxy_manifest_sha256},
        @{name="pi_adapter";actual=$piManifestHash;expected=[string]$receipt.runtime_integrity.pi_adapter_manifest_sha256}
    )){if([string]::IsNullOrWhiteSpace($pair.expected)-or$pair.actual-ne$pair.expected){throw "Installed receipt integrity mismatch for $($pair.name)."}}
}

$pythonEnvironment=@("PYTHONPATH","PYTHONHOME","PYTHONSTARTUP","PYTHONNOUSERSITE","PYTHONDONTWRITEBYTECODE")
$previousPythonEnvironment=@{}
try{
    foreach($name in $pythonEnvironment){$previousPythonEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,"Process");[Environment]::SetEnvironmentVariable($name,$null,"Process")}
    [Environment]::SetEnvironmentVariable("PYTHONNOUSERSITE","1","Process")
    [Environment]::SetEnvironmentVariable("PYTHONDONTWRITEBYTECODE","1","Process")
    $pythonVersion=(& $runtimePython -I -B -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')" 2>&1|Out-String).Trim()
    if($LASTEXITCODE-ne0-or$pythonVersion-ne[string]$runtime.python_version){throw "jCodeMunch Python version mismatch."}
    $versionOutput=(& $runtimePython -I -B -c "import importlib.metadata; print(importlib.metadata.version('jcodemunch-mcp'))" 2>&1|Out-String).Trim()
    if($LASTEXITCODE-ne0-or$versionOutput-ne[string]$runtime.version){throw "jCodeMunch version mismatch."}
    $helpOutput=(& $runtimePython -I -B -m jcodemunch_mcp serve --help 2>&1|Out-String).ToLowerInvariant()
    if($LASTEXITCODE-ne0-or$helpOutput-notmatch"streamable-http"){throw "jCodeMunch does not advertise Streamable HTTP."}
}finally{foreach($name in $pythonEnvironment){[Environment]::SetEnvironmentVariable($name,$previousPythonEnvironment[$name],"Process")}}

[ordered]@{
    jcodemunch=[ordered]@{package=$runtime.package;version=$runtime.version;executable=$runtimePython;sha256=$runtime.sha256;manifest_sha256=$runtimeManifestHash;installed_code_verified=$true;transport=$runtime.transport;bind_host=$runtime.bind_host}
    stdio_proxy=[ordered]@{package=$proxy.package;version=$proxy.version;entrypoint=$proxyEntrypoint;integrity=$proxy.integrity;manifest_sha256=$proxyManifestHash;installed_code_verified=$true;transport=$proxy.transport}
    pi_adapter=[ordered]@{package=$pi.package;version=$pi.version;integrity=$pi.integrity;artifact=$piArtifact;manifest_sha256=$piManifestHash}
    integration_assets=if($null-eq$integrationManifestHash){$null}else{[ordered]@{root=[IO.Path]::GetFullPath($IntegrationRoot);schema_version=1;manifest_sha256=$integrationManifestHash;file_count=[int]$integrationValidation.file_count;verified=$true}}
    verified=$true
}|ConvertTo-Json -Depth 8 -Compress
