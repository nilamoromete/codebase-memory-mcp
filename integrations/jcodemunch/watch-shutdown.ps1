#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$LifecycleScript,
    [Parameter(Mandatory=$true)][string]$StateRoot,
    [Parameter(Mandatory=$true)][string]$ConfigRoot,
    [Parameter(Mandatory=$true)][string]$Generation,
    [ValidateRange(50,5000)][int]$PollMilliseconds=100
)

Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
$modulePath=Join-Path (Split-Path -Parent $LifecycleScript) "JCodeMunchLifecycle.psm1"
Import-Module $modulePath -Force

while($true){
    try {$shutdown=Read-JCodeMunchStateDocument -Name shutdown -StateRoot $StateRoot}
    catch {exit 0}
    if($null -eq $shutdown -or [string]$shutdown.generation -ne $Generation){exit 0}
    if($null -eq $shutdown.PSObject.Properties["armed"] -or -not [bool]$shutdown.armed){
        Start-Sleep -Milliseconds $PollMilliseconds
        continue
    }
    try {
        $shutdownAt=if($shutdown.shutdown_at -is [DateTime]){
            ([DateTime]$shutdown.shutdown_at).ToUniversalTime()
        }else{
            [DateTimeOffset]::Parse(
                [string]$shutdown.shutdown_at,
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::RoundtripKind
            ).UtcDateTime
        }
    }
    catch {exit 0}
    $remaining=[int][Math]::Ceiling(($shutdownAt-[DateTime]::UtcNow).TotalMilliseconds)
    if($remaining -le 0){break}
    Start-Sleep -Milliseconds ([Math]::Min($PollMilliseconds,$remaining))
}

& $LifecycleScript -Command clean -StateRoot $StateRoot -ConfigRoot $ConfigRoot -ExpectedShutdownGeneration $Generation -Json | Out-Null
exit $LASTEXITCODE
