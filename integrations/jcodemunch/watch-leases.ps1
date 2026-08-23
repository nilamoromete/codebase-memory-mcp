#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$LifecycleScript,
    [Parameter(Mandatory=$true)][string]$StateRoot,
    [Parameter(Mandatory=$true)][string]$ConfigRoot,
    [Parameter(Mandatory=$true)][string]$OwnerId,
    [Parameter(Mandatory=$true)][string]$Generation,
    [ValidateRange(0,86400)][int]$GraceSeconds=600,
    [ValidateRange(50,5000)][int]$PollMilliseconds=500
)

Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
$modulePath=Join-Path (Split-Path -Parent $LifecycleScript) "JCodeMunchLifecycle.psm1"
Import-Module $modulePath -Force
$zeroSince=$null

function Test-LeaseProcessLive {
    param([psobject]$Lease)
    if($null -eq $Lease.PSObject.Properties["pid"] -or $null -eq $Lease.PSObject.Properties["process_start_time"]){return $null}
    $leasePid=0
    if(-not[int]::TryParse([string]$Lease.pid,[ref]$leasePid)-or$leasePid-lt1){return $null}
    try{$process=Get-Process -Id $leasePid -ErrorAction Stop}catch{return $false}
    try{$start="ticks:$($process.StartTime.ToUniversalTime().Ticks)"}catch{return $null}
    return $start -eq [string]$Lease.process_start_time
}
try {
    while($true){
        Start-Sleep -Milliseconds $PollMilliseconds
        try{$owner=Read-JCodeMunchStateDocument -Name owner -StateRoot $StateRoot}catch{break}
        if($null -eq $owner -or [string]$owner.owner_id -ne $OwnerId){break}
        try{$leaseState=Read-JCodeMunchStateDocument -Name leases -StateRoot $StateRoot}catch{$leaseState=$null}
        $leases=@()
        if($null -ne $leaseState){$leases=@($leaseState.leases)}
        $hasProvenStale=@($leases|Where-Object{(Test-LeaseProcessLive $_) -eq $false}).Count -gt 0
        if($hasProvenStale){
            $raw=& $LifecycleScript -Command clean -StateRoot $StateRoot -ConfigRoot $ConfigRoot -Json
            if($LASTEXITCODE -ne 0){continue}
            try{$cleanResult=$raw|ConvertFrom-Json -ErrorAction Stop}catch{continue}
            if($null -ne $cleanResult.PSObject.Properties["remaining_leases"] -and [int]$cleanResult.remaining_leases -gt 0){$zeroSince=$null;continue}
            try{$leaseState=Read-JCodeMunchStateDocument -Name leases -StateRoot $StateRoot}catch{$leaseState=$null}
            $leases=@()
            if($null -ne $leaseState){$leases=@($leaseState.leases)}
        }
        if($leases.Count -gt 0){$zeroSince=$null;continue}
        if($null -eq $zeroSince){$zeroSince=[DateTime]::UtcNow}
        if(([DateTime]::UtcNow-$zeroSince).TotalSeconds -lt $GraceSeconds){continue}
        $raw=& $LifecycleScript -Command clean -StateRoot $StateRoot -ConfigRoot $ConfigRoot -Now -Json
        if($LASTEXITCODE -ne 0){$zeroSince=$null;continue}
        try{$cleanResult=$raw|ConvertFrom-Json -ErrorAction Stop}catch{$zeroSince=$null;continue}
        if([string]$cleanResult.server_action -in @("stop","none")){break}
        $zeroSince=$null
    }
}
catch {
    try {Write-JCodeMunchDiagnosticLog -Event "lease_monitor_failed" -Level "error" -Details @{message=$_.Exception.Message;owner_id=$OwnerId;generation=$Generation} -StateRoot $StateRoot}catch{}
}
finally {
    try {
        Invoke-JCodeMunchStateLock -StateRoot $StateRoot -ScriptBlock {
            $monitor=Read-JCodeMunchStateDocument -Name monitor -StateRoot $StateRoot
            if($null -ne $monitor -and [string]$monitor.generation -eq $Generation){
                Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name monitor -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue
            }
        }|Out-Null
    } catch {}
}
