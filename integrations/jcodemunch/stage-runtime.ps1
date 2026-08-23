#Requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$DestinationRoot,[string]$ReceiptPath,[string]$Python="python",[string]$Npm="npm")
Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
Import-Module (Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1") -Force
$manifest=Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot "versions.json")|ConvertFrom-Json
$runtime=$manifest.jcodemunch;$proxy=$manifest.stdio_proxy;$pi=$manifest.pi_adapter
$requirementsPath=Join-Path $PSScriptRoot ([string]$runtime.requirements_lock)
if(-not(Test-Path -LiteralPath $requirementsPath -PathType Leaf)-or(Get-FileHash -LiteralPath $requirementsPath -Algorithm SHA256).Hash.ToLowerInvariant()-ne([string]$runtime.requirements_sha256).ToLowerInvariant()){throw "Pinned Python dependency lock integrity check failed."}
if(-not[OperatingSystem]::IsWindows()-or[Runtime.InteropServices.RuntimeInformation]::OSArchitecture-ne[Runtime.InteropServices.Architecture]::X64){throw "The pinned jCodeMunch runtime currently supports Windows x64 only."}
$root=[IO.Path]::GetFullPath($DestinationRoot)
Assert-JCodeMunchNoReparseAncestors $root|Out-Null
if($runtime.transport-ne"streamable-http"-or$runtime.bind_host-ne"127.0.0.1"-or-not$runtime.bearer_auth){throw "Refusing to stage jCodeMunch without authenticated loopback."}
if($proxy.transport-ne"http-only"){throw "Refusing to stage a fallback-capable STDIO proxy."}
New-Item -ItemType Directory -Path $root -Force|Out-Null
$stagingRoot=Join-Path $root (".staging-"+[Guid]::NewGuid().ToString("N"))
$runtimeStage=Join-Path $stagingRoot "jcodemunch-mcp";$proxyStage=Join-Path $stagingRoot "mcp-remote";$piStage=Join-Path $stagingRoot "pi-mcp-extension"
$runtimeFinal=Join-Path (Join-Path $root ([string]$runtime.package)) ([string]$runtime.version)
$proxyFinal=Join-Path (Join-Path $root ([string]$proxy.package)) ([string]$proxy.version)
$piFinal=Join-Path (Join-Path $root ([string]$pi.package)) ([string]$pi.version)

function Assert-SafeChild([string]$Path){
    $full=[IO.Path]::GetFullPath($Path);$prefix=$root.TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
    if(-not$full.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw "Refusing filesystem action outside runtime root: $full"}
    Assert-JCodeMunchNoReparseAncestors $root|Out-Null
    Assert-JCodeMunchNoReparseAncestors $full|Out-Null
    return $full
}
function Get-ScopeEntries([string]$Base,[string[]]$Scopes){
    $entries=[Collections.Generic.List[object]]::new()
    foreach($scope in $Scopes){
        $scopeRoot=Join-Path $Base $scope
        if(-not(Test-Path -LiteralPath $scopeRoot -PathType Container)){throw "Integrity scope is missing: $scopeRoot"}
        $pending=[Collections.Generic.Stack[string]]::new();$pending.Push([IO.Path]::GetFullPath($scopeRoot))
        while($pending.Count-gt0){$directory=$pending.Pop();$directoryItem=Get-Item -LiteralPath $directory -Force;if(($directoryItem.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Integrity scope contains a link or reparse point: $directory"};foreach($child in @(Get-ChildItem -LiteralPath $directory -Force)){if(($child.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Integrity scope contains a link or reparse point: $($child.FullName)"};if($child.PSIsContainer){$pending.Push($child.FullName)}}}
        foreach($file in @(Get-ChildItem -LiteralPath $scopeRoot -File -Recurse|Sort-Object FullName)){
            if($file.Extension-eq'.pyc'){throw "Executable Python bytecode is forbidden in the published runtime: $($file.FullName)"}
            $relative=$file.FullName.Substring($Base.Length).TrimStart([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar).Replace('\','/')
            $entries.Add([ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()})
        }
    }
    return @($entries)
}
function Write-ComponentManifest([string]$Base,[string]$Name,[string]$Version,[string[]]$Scopes){
    $document=[ordered]@{schema_version=1;component=$Name;version=$Version;scopes=$Scopes;files=@(Get-ScopeEntries $Base $Scopes);generated_at=[DateTime]::UtcNow.ToString("o")}
    [IO.File]::WriteAllText((Join-Path $Base "installation-manifest.json"),($document|ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
}

# Fail before network or filesystem mutation when an existing package parent
# redirects through a junction/symlink.
foreach($parent in @((Split-Path -Parent $runtimeFinal),(Split-Path -Parent $proxyFinal),(Split-Path -Parent $piFinal))){Assert-SafeChild $parent|Out-Null}

$existing=@(@($runtimeFinal,$proxyFinal,$piFinal)|Where-Object{Test-Path -LiteralPath $_ -PathType Container})
if($existing.Count-gt0){
    if($existing.Count-ne3){throw "Refusing to reuse a partially published companion composition. Roll it back or remove only the recorded owned runtime paths."}
    if([string]::IsNullOrWhiteSpace($ReceiptPath)){throw "Refusing runtime reuse without the existing installation receipt."}
    $verified=& (Join-Path $PSScriptRoot "verify-runtime.ps1") -RuntimeRoot $root -ReceiptPath $ReceiptPath
    if($LASTEXITCODE-ne0){throw "Existing companion composition failed validation."}
    [ordered]@{verified=$true;reused=$true;published_paths=@();components=($verified|ConvertFrom-Json)}|ConvertTo-Json -Depth 15 -Compress
    return
}

$published=[Collections.Generic.List[string]]::new()
$stagingEnvironment=@("PYTHONPATH","PYTHONHOME","PYTHONSTARTUP","PYTHONNOUSERSITE","PYTHONDONTWRITEBYTECODE","NODE_OPTIONS","NODE_PATH")
$previousStagingEnvironment=@{};foreach($name in $stagingEnvironment){$previousStagingEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,"Process")}
try{
    foreach($name in @("PYTHONPATH","PYTHONHOME","PYTHONSTARTUP")){[Environment]::SetEnvironmentVariable($name,$null,"Process")}
    foreach($name in @("NODE_OPTIONS","NODE_PATH")){[Environment]::SetEnvironmentVariable($name,$null,"Process")}
    [Environment]::SetEnvironmentVariable("PYTHONNOUSERSITE","1","Process")
    [Environment]::SetEnvironmentVariable("PYTHONDONTWRITEBYTECODE","1","Process")
    New-Item -ItemType Directory -Path $runtimeStage,$proxyStage,$piStage -Force|Out-Null
    $wheelPath=Join-Path $runtimeStage ([string]$runtime.artifact)
    Invoke-WebRequest -Uri ([string]$runtime.source) -OutFile $wheelPath
    if((Get-FileHash -LiteralPath $wheelPath -Algorithm SHA256).Hash.ToLowerInvariant()-ne([string]$runtime.sha256).ToLowerInvariant()){throw "Pinned jCodeMunch artifact hash mismatch."}
    $basePythonVersion=(& $Python -I -B -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')" 2>&1|Out-String).Trim()
    if($LASTEXITCODE-ne0-or$basePythonVersion-ne[string]$runtime.python_version){throw "Pinned Python version mismatch; expected $($runtime.python_version), got $basePythonVersion."}
    $venvPath=Join-Path $runtimeStage "venv";$venvOutput=@(& $Python -I -B -m venv $venvPath 2>&1);if($LASTEXITCODE-ne0){throw "Python virtual environment creation failed: $($venvOutput-join[Environment]::NewLine)"}
    $venvPython=Join-Path $venvPath "Scripts\python.exe"
    $dependencyOutput=@(& $venvPython -I -B -m pip install --disable-pip-version-check --only-binary=:all: --require-hashes -r $requirementsPath 2>&1);if($LASTEXITCODE-ne0){throw "Pinned jCodeMunch dependency installation failed: $($dependencyOutput-join[Environment]::NewLine)"}
    $pipOutput=@(& $venvPython -I -B -m pip install --disable-pip-version-check --no-deps "$wheelPath[http]" 2>&1);if($LASTEXITCODE-ne0){throw "Pinned jCodeMunch installation failed: $($pipOutput-join[Environment]::NewLine)"}
    $pipCheck=@(& $venvPython -I -B -m pip check 2>&1);if($LASTEXITCODE-ne0){throw "Pinned jCodeMunch dependency closure is incomplete: $($pipCheck-join[Environment]::NewLine)"}
    $versionOutput=(& $venvPython -I -B -c "import importlib.metadata; print(importlib.metadata.version('jcodemunch-mcp'))" 2>&1|Out-String).Trim()
    if($LASTEXITCODE-ne0-or$versionOutput-ne[string]$runtime.version){throw "Staged jCodeMunch version verification failed."}
    $distInfo=(Get-ChildItem -LiteralPath (Join-Path $venvPath "Lib\site-packages") -Directory -Filter "jcodemunch_mcp-*.dist-info"|Select-Object -First 1).Name
    if([string]::IsNullOrWhiteSpace($distInfo)){throw "Staged jCodeMunch metadata is missing."}
    foreach($bytecode in @(Get-ChildItem -LiteralPath $venvPath -File -Recurse -Filter "*.pyc")){
        $bytecodeFull=[IO.Path]::GetFullPath($bytecode.FullName);$venvPrefix=[IO.Path]::GetFullPath($venvPath).TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
        if(-not$bytecodeFull.StartsWith($venvPrefix,[StringComparison]::OrdinalIgnoreCase)){throw "Refusing bytecode cleanup outside the staged venv: $bytecodeFull"}
        Remove-Item -LiteralPath $bytecodeFull -Force -ErrorAction Stop
    }
    # Attest the complete virtual environment, including the interpreter,
    # launchers and pyvenv.cfg. Hashing site-packages alone would leave the
    # executable that actually loads those packages outside the trust boundary.
    Write-ComponentManifest $runtimeStage ([string]$runtime.package) ([string]$runtime.version) @("venv")

    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "proxy\package.json") -Destination (Join-Path $proxyStage "package.json")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "proxy\package-lock.json") -Destination (Join-Path $proxyStage "package-lock.json")
    $npmOutput=@(& $Npm ci --ignore-scripts --no-audit --no-fund --prefix $proxyStage 2>&1);if($LASTEXITCODE-ne0){throw "Pinned mcp-remote installation failed: $($npmOutput-join[Environment]::NewLine)"}
    $installedProxy=Get-Content -LiteralPath (Join-Path $proxyStage "node_modules\mcp-remote\package.json") -Raw|ConvertFrom-Json
    if([string]$installedProxy.version-ne[string]$proxy.version){throw "Staged proxy version verification failed."}
    Write-ComponentManifest $proxyStage ([string]$proxy.package) ([string]$proxy.version) @("node_modules")

    $piArtifact="$($pi.package)-$($pi.version).tgz";$piArtifactPath=Join-Path $piStage $piArtifact
    Invoke-WebRequest -Uri ([string]$pi.source) -OutFile $piArtifactPath
    $expectedIntegrity=([string]$pi.integrity)-replace'^sha512-',''
    $actualIntegrity=[Convert]::ToBase64String([Security.Cryptography.SHA512]::HashData([IO.File]::ReadAllBytes($piArtifactPath)))
    if($actualIntegrity-ne$expectedIntegrity){throw "Pinned Pi extension integrity mismatch."}
    [IO.File]::WriteAllText((Join-Path $piStage "installation-manifest.json"),([ordered]@{schema_version=1;component=[string]$pi.package;version=[string]$pi.version;integrity=[string]$pi.integrity;artifact=$piArtifact;generated_at=[DateTime]::UtcNow.ToString("o")}|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))

    foreach($item in @(@{stage=$runtimeStage;final=$runtimeFinal},@{stage=$proxyStage;final=$proxyFinal},@{stage=$piStage;final=$piFinal})){
        $safeFinal=Assert-SafeChild $item.final;$parent=Split-Path -Parent $safeFinal;New-Item -ItemType Directory -Path $parent -Force|Out-Null
        [IO.Directory]::Move([string]$item.stage,$safeFinal);$published.Add($safeFinal)
    }
    $verified=& (Join-Path $PSScriptRoot "verify-runtime.ps1") -RuntimeRoot $root
    if($LASTEXITCODE-ne0){throw "Published companion composition failed validation."}
    [ordered]@{verified=$true;reused=$false;published_paths=@($published);components=($verified|ConvertFrom-Json)}|ConvertTo-Json -Depth 15 -Compress
}catch{
    $cause=$_.Exception.Message;$cleanupFailures=[Collections.Generic.List[string]]::new()
    $cleanupPaths=@($published)
    [array]::Reverse($cleanupPaths)
    foreach($path in $cleanupPaths){
        try{$safe=Assert-SafeChild $path;if(Test-Path -LiteralPath $safe -PathType Container){Remove-Item -LiteralPath $safe -Recurse -Force -ErrorAction Stop}}catch{$cleanupFailures.Add($_.Exception.Message)}
    }
    if($cleanupFailures.Count){throw "$cause Atomic publication cleanup failed: $($cleanupFailures-join'; ')"}
    throw $cause
}finally{
    foreach($name in $stagingEnvironment){[Environment]::SetEnvironmentVariable($name,$previousStagingEnvironment[$name],"Process")}
    if(Test-Path -LiteralPath $stagingRoot -PathType Container){$safe=Assert-SafeChild $stagingRoot;Remove-Item -LiteralPath $safe -Recurse -Force -ErrorAction SilentlyContinue}
}
