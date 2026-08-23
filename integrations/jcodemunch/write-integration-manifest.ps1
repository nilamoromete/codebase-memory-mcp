#Requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$IntegrationRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
Import-Module (Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1") -Force
$root=[IO.Path]::GetFullPath($IntegrationRoot)
Assert-JCodeMunchNoReparseAncestors $root|Out-Null
if(-not(Test-Path -LiteralPath $root -PathType Container)){throw "Integration root is missing: $root"}
$manifestPath=Join-Path $root "integration-manifest.json"
$pending=[Collections.Generic.Stack[string]]::new();$pending.Push($root)
while($pending.Count-gt0){
    $directory=$pending.Pop();$item=Get-Item -LiteralPath $directory -Force
    if(($item.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Integration root contains a link or reparse point: $directory"}
    foreach($child in @(Get-ChildItem -LiteralPath $directory -Force)){
        if(($child.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw "Integration root contains a link or reparse point: $($child.FullName)"}
        if($child.PSIsContainer){$pending.Push($child.FullName)}
    }
}
$files=[Collections.Generic.List[object]]::new()
foreach($file in @(Get-ChildItem -LiteralPath $root -File -Recurse|Where-Object{-not[string]::Equals($_.FullName,$manifestPath,[StringComparison]::OrdinalIgnoreCase)}|Sort-Object FullName)){
    if($file.Extension-eq'.pyc'){throw "Executable Python bytecode is forbidden in integration assets: $($file.FullName)"}
    $relative=$file.FullName.Substring($root.Length).TrimStart([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar).Replace('\','/')
    $files.Add([ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()})
}
if($files.Count-lt1){throw "Integration root contains no attestable assets."}
$document=[ordered]@{schema_version=1;product="codebase-memory-mcp-hybrid-integration";files=@($files);generated_at=[DateTime]::UtcNow.ToString("o")}
Write-JCodeMunchAtomicText -Path $manifestPath -Text ($document|ConvertTo-Json -Depth 10) -Secret
[ordered]@{path=$manifestPath;manifest_sha256=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant();file_count=$files.Count;verified=$true}|ConvertTo-Json -Compress
