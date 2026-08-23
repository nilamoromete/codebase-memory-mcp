#Requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidateSet("configure","acquire","release","heartbeat","status","stop","clean","rollback")][string]$Command,[string]$StateRoot,[string]$ConfigRoot,[string]$Client,[string]$SessionId,[string]$WorkspaceRoot,[int]$ClientPid=0,[string[]]$CandidatePorts,[string[]]$CandidateRanges,[string[]]$DenylistedPorts,[string[]]$ReservedRanges,[string[]]$DynamicRanges,[string[]]$ExcludedRanges,[string]$CodexConfig,[string]$ClaudeConfig,[string]$ClaudeSettingsConfig,[string]$PiConfig,[string]$PiSettingsConfig,[string]$IntegrationRoot,[string[]]$SourceAssetPaths=@(),[switch]$ApplyClientConfigs,[int]$FailureInjectionAfterWrites=0,[int]$GraceSeconds=600,[string]$ExpectedShutdownGeneration,[switch]$Now,[switch]$ReselectPort,[string]$OwnershipFixture,[switch]$Json)
Set-StrictMode -Version Latest
$ErrorActionPreference="Stop"
$RuntimeStartupTimeoutMilliseconds=30000
if ([string]::IsNullOrWhiteSpace($StateRoot)) { if ([Environment]::GetEnvironmentVariable("JCODEMUNCH_LIFECYCLE_TEST_MODE") -eq "1") { throw "StateRoot is required in TEST_MODE." }; $StateRoot=Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "CodebaseMemoryMcp\companions\jcodemunch" }
if ([string]::IsNullOrWhiteSpace($ConfigRoot)) { $ConfigRoot=$StateRoot }
Import-Module (Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1") -Force
function Throw-LifecycleError { param([string]$Code,[string]$Message,[hashtable]$Details=@{}); $e=[Exception]::new($Message);$e.Data["code"]=$Code;foreach($k in $Details.Keys){$e.Data[$k]=$Details[$k]};throw $e }
function Parse-Ports { param([string[]]$Values);$r=[Collections.Generic.List[int]]::new();foreach($v in @($Values)){foreach($p in ([string]$v -split ",")){if([string]::IsNullOrWhiteSpace($p)){continue};$n=0;if(-not [int]::TryParse($p.Trim(),[ref]$n)){Throw-LifecycleError "invalid_port" "Invalid port value '$p'."};$r.Add($n)}};return @($r) }
function Get-WorkspaceIdentity {
    param([string]$Path)
    $candidate=if([string]::IsNullOrWhiteSpace($Path)){(Get-Location).Path}else{$Path}
    try {
        $full=[IO.Path]::GetFullPath($candidate)
        $resolved=Resolve-Path -LiteralPath $full -ErrorAction SilentlyContinue
        if($null -ne $resolved){$full=[string]$resolved.Path}
    } catch {
        Throw-LifecycleError "invalid_workspace" "Workspace root '$candidate' is not a valid path."
    }
    $pathRoot=[IO.Path]::GetPathRoot($full)
    if($full.Length -gt $pathRoot.Length){$full=$full.TrimEnd([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar)}
    $hashInput=if([OperatingSystem]::IsWindows()){$full.ToUpperInvariant()}else{$full}
    $digest=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($hashInput))).ToLowerInvariant()
    return [pscustomobject]@{workspace_root=$full;workspace_id="sha256:$digest"}
}
function ConvertTo-JCodeMunchUtc {
    param([Parameter(Mandatory=$true)][object]$Value)
    if($Value -is [DateTime]){return ([DateTime]$Value).ToUniversalTime()}
    return [DateTimeOffset]::Parse(
        [string]$Value,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind
    ).UtcDateTime
}
function Parse-Ranges {
    param([string[]]$Values,[string]$Source="operator")
    $ranges=[Collections.Generic.List[object]]::new()
    foreach($value in @($Values)){foreach($part in ([string]$value -split ",")){if([string]::IsNullOrWhiteSpace($part)){continue};$bounds=$part.Trim() -split "-",2;$start=0;$end=0;if($bounds.Count -ne 2 -or -not [int]::TryParse($bounds[0].Trim(),[ref]$start) -or -not [int]::TryParse($bounds[1].Trim(),[ref]$end) -or $start -lt 1 -or $end -gt 65535 -or $start -gt $end){Throw-LifecycleError "invalid_range" "Invalid port range '$part'." @{source=$Source}};$ranges.Add([pscustomobject]@{start=$start;end=$end;source=$Source})}}
    return @($ranges)
}
function Get-WindowsDynamicRanges {
    if($null -eq (Get-Command Get-NetTCPSetting -ErrorAction SilentlyContinue)){Throw-LifecycleError "range_discovery_unavailable" "Windows TCP dynamic-port range discovery is unavailable." @{source="dynamic"}}
    try{$rows=@(Get-NetTCPSetting -ErrorAction Stop|Where-Object{$null -ne $_.DynamicPortRangeStartPort -and $null -ne $_.DynamicPortRangeNumberOfPorts});$ranges=[Collections.Generic.List[object]]::new();foreach($row in $rows){$start=[int]$row.DynamicPortRangeStartPort;$count=[int]$row.DynamicPortRangeNumberOfPorts;if($count -lt 1){throw "Malformed dynamic TCP range count."};$end=$start+$count-1;if($start -lt 1 -or $end -gt 65535){throw "Malformed dynamic TCP range bounds."};$ranges.Add([pscustomobject]@{start=$start;end=$end;source="windows-dynamic-ipv4-ipv6"})};if($ranges.Count -eq 0){throw "No dynamic TCP ranges were returned."};return @($ranges|Sort-Object start,end -Unique)}catch{Throw-LifecycleError "range_discovery_unavailable" "Windows TCP dynamic-port range discovery returned malformed data." @{source="dynamic"}}
}
function Get-WindowsExcludedRanges {
    if($null -eq (Get-Command netsh.exe -ErrorAction SilentlyContinue)){Throw-LifecycleError "range_discovery_unavailable" "Windows TCP excluded-port range discovery is unavailable." @{source="excluded"}}
    $ranges=[Collections.Generic.List[object]]::new();foreach($family in @("ipv4","ipv6")){try{$lines=@(& netsh.exe interface $family show excludedportrange protocol=tcp 2>$null);if($LASTEXITCODE -ne 0){throw "netsh failed"};$recognized=$false;foreach($line in $lines){$match=[regex]::Match([string]$line,'^\s*(\d+)\s+(\d+)\s*$');if($match.Success){$recognized=$true;$start=[int]$match.Groups[1].Value;$end=[int]$match.Groups[2].Value;if($start -lt 1 -or $end -gt 65535 -or $start -gt $end){throw "Malformed excluded range."};$ranges.Add([pscustomobject]@{start=$start;end=$end;source="windows-excluded-$family"})}};if(-not $recognized -and -not ([string]::Join(" ",$lines)-match '(?i)no excluded|excluded port ranges')){throw "Unrecognized excluded-range output."}}catch{Throw-LifecycleError "range_discovery_unavailable" "Windows TCP excluded-port range discovery returned malformed data." @{source="excluded-$family"}}};return @($ranges)
}
function Get-PortRangePolicy {
    $onWindows=[Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT;$testMode=[Environment]::GetEnvironmentVariable("JCODEMUNCH_LIFECYCLE_TEST_MODE") -eq "1";if($testMode -and [Environment]::GetEnvironmentVariable("JCODEMUNCH_RANGE_DISCOVERY") -eq "unavailable"){Throw-LifecycleError "range_discovery_unavailable" "TCP range discovery was unavailable."}
    $dynamic=if(@($DynamicRanges).Count){Parse-Ranges $DynamicRanges "operator-dynamic"}elseif($onWindows){Get-WindowsDynamicRanges}else{@([pscustomobject]@{start=49152;end=65535;source="non-windows-safe-default"})}
    $excluded=if(@($ExcludedRanges).Count){Parse-Ranges $ExcludedRanges "operator-excluded"}elseif($onWindows){Get-WindowsExcludedRanges}else{@()}
    $reserved=@(Parse-Ranges $ReservedRanges "manager-reserved");$denylisted=[Collections.Generic.List[object]]::new();foreach($port in @(Parse-Ports $DenylistedPorts)){$denylisted.Add([pscustomobject]@{start=[int]$port;end=[int]$port;source="operator-denylist"})}
    return [pscustomobject]@{dynamic_ranges=@($dynamic);excluded_ranges=@($excluded);reserved_ranges=@($reserved);denylisted_ranges=@($denylisted);candidate_ranges=@(Parse-Ranges $CandidateRanges "candidate")}
}
function Get-ActiveLocalTcpPorts {
    if([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or $null -eq (Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue)){return @()};try{return @(Get-NetTCPConnection -ErrorAction Stop|ForEach-Object{[pscustomobject]@{port=[int]$_.LocalPort;owning_pid=if($null -ne $_.OwningProcess){[int]$_.OwningProcess}else{$null};address_family=[string]$_.AddressFamily}})}catch{return @()}
}
function Test-PortAvailable {
    param([int]$Port)
    if($Port -lt 1024 -or $Port -gt 65535){return [pscustomobject]@{available=$false;reason="invalid_port";port=$Port}};$active=@(Get-ActiveLocalTcpPorts|Where-Object{[int]$_.port -eq $Port});if($active.Count -gt 0){return [pscustomobject]@{available=$false;reason="occupied";port=$Port;occupants=$active}}
    $listeners=[Collections.Generic.List[object]]::new();$addresses=@([pscustomobject]@{address="127.0.0.1";family="ipv4"},[pscustomobject]@{address="::1";family="ipv6"});try{foreach($item in $addresses){$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Parse($item.address),$Port);$listener.Server.ExclusiveAddressUse=$true;if($item.family -eq "ipv6"){$listener.Server.DualMode=$false};$listener.Start();$listeners.Add($listener)};return [pscustomobject]@{available=$true;port=$Port}}catch{$failed=$addresses[$listeners.Count];return [pscustomobject]@{available=$false;reason="occupied";port=$Port;address_family=$failed.family;message=$_.Exception.Message}}finally{foreach($listener in $listeners){$listener.Stop()}}
}function Read-State { param([string]$Name);Read-JCodeMunchStateDocument -Name $Name -StateRoot $StateRoot }
function Write-State { param([string]$Name,[psobject]$Document);Write-JCodeMunchStateDocument -Name $Name -Document $Document -StateRoot $StateRoot -LockHeld|Out-Null }
function Write-Diagnostic { param([string]$Event,[string]$Level="info",[object]$Details);Write-JCodeMunchDiagnosticLog -Event $Event -Level $Level -Details $Details -StateRoot $StateRoot }
function Get-ProcessSnapshot {
    param([int]$ProcessId)
    try {
        $process=Get-Process -Id $ProcessId -ErrorAction Stop
        $start="ticks:$($process.StartTime.ToUniversalTime().Ticks)"
        $executable=$null
        try { $executable=[string]$process.Path } catch {}
        $command=$null
        if($null -ne (Get-Command Get-CimInstance -ErrorAction SilentlyContinue)) {
            try {
                $cim=Get-CimInstance Win32_Process -Filter "ProcessId = $ProcessId" -ErrorAction Stop | Select-Object -First 1
                if($null -ne $cim) {
                    if(-not [string]::IsNullOrWhiteSpace([string]$cim.ExecutablePath)) { $executable=[string]$cim.ExecutablePath }
                    if(-not [string]::IsNullOrWhiteSpace([string]$cim.CommandLine)) { $command=[string]$cim.CommandLine }
                }
            } catch {}
        }
        if([string]::IsNullOrWhiteSpace($executable) -and [string]::IsNullOrWhiteSpace($command)) { return $null }
        return [pscustomobject]@{pid=$ProcessId;process_start_time=$start;executable_path=$executable;command_line=$command}
    } catch { return $null }
}
function Get-ProcessStart {
    param([int]$ProcessId)
    try {
        $process=Get-Process -Id $ProcessId -ErrorAction Stop
        return "ticks:$($process.StartTime.ToUniversalTime().Ticks)"
    } catch {return $null}
}
function Test-ProcessSnapshotMatch {
    param([AllowNull()][psobject]$Snapshot)
    if($null -eq $Snapshot){return $false}
    $current=Get-ProcessSnapshot ([int]$Snapshot.pid)
    if($null -eq $current -or [string]$current.process_start_time -ne [string]$Snapshot.process_start_time){return $false}
    foreach($field in @("executable_path","command_line")){
        if($null -ne $Snapshot.PSObject.Properties[$field] -and -not [string]::IsNullOrWhiteSpace([string]$Snapshot.$field) -and [string]$current.$field -ne [string]$Snapshot.$field){return $false}
    }
    return $true
}
function Test-ProcessDescendant {
    param([int]$ChildProcessId,[int]$AncestorProcessId)
    if($ChildProcessId -lt 1 -or $AncestorProcessId -lt 1 -or $ChildProcessId -eq $AncestorProcessId){return $false}
    $current=$ChildProcessId
    $seen=[Collections.Generic.HashSet[int]]::new()
    for($depth=0;$depth -lt 8;$depth++){
        if(-not$seen.Add($current)){return $false}
        try{$row=Get-CimInstance Win32_Process -Filter "ProcessId = $current" -ErrorAction Stop|Select-Object -First 1}catch{return $false}
        if($null -eq $row){return $false}
        $parent=[int]$row.ParentProcessId
        if($parent -eq $AncestorProcessId){return $true}
        if($parent -lt 1 -or $parent -eq $current){return $false}
        $current=$parent
    }
    return $false
}
function Get-LaunchedProcessDescendants {
    param([Parameter(Mandatory=$true)][psobject]$LauncherSnapshot)
    if(-not(Test-ProcessSnapshotMatch $LauncherSnapshot)){return @()}
    try{$rows=@(Get-CimInstance Win32_Process -ErrorAction Stop)}catch{return @()}
    $frontier=@([int]$LauncherSnapshot.pid)
    $seen=[Collections.Generic.HashSet[int]]::new();[void]$seen.Add([int]$LauncherSnapshot.pid)
    $result=[Collections.Generic.List[object]]::new()
    for($depth=1;$depth -le 16 -and $frontier.Count -gt 0;$depth++){
        $next=[Collections.Generic.List[int]]::new()
        foreach($row in $rows){
            $pidValue=[int]$row.ProcessId;$parentValue=[int]$row.ParentProcessId
            if($frontier -notcontains $parentValue -or -not $seen.Add($pidValue)){continue}
            $snapshot=Get-ProcessSnapshot $pidValue
            if($null -ne $snapshot){$result.Add([pscustomobject]@{snapshot=$snapshot;depth=$depth})}
            $next.Add($pidValue)
        }
        $frontier=@($next)
    }
    return @($result)
}
function Stop-LaunchedProcessTree {
    param(
        [Parameter(Mandatory=$true)][psobject]$LauncherSnapshot,
        [object[]]$KnownDescendants=@(),
        [string]$StopPath
    )
    try {
        if(-not [string]::IsNullOrWhiteSpace($StopPath)){New-Item -ItemType File -Path $StopPath -Force|Out-Null}
        $candidates=@{}
        foreach($entry in @($KnownDescendants)+@(Get-LaunchedProcessDescendants $LauncherSnapshot)){
            if($null -eq $entry -or $null -eq $entry.snapshot){continue}
            $key="$([int]$entry.snapshot.pid)|$([string]$entry.snapshot.process_start_time)"
            $candidates[$key]=$entry
        }
        $clock=[Diagnostics.Stopwatch]::StartNew()
        while($clock.ElapsedMilliseconds -lt 1500 -and (Test-ProcessSnapshotMatch $LauncherSnapshot)){Start-Sleep -Milliseconds 50}
        foreach($entry in @($candidates.Values|Sort-Object depth -Descending)){
            if(Test-ProcessSnapshotMatch $entry.snapshot){Stop-Process -Id ([int]$entry.snapshot.pid) -Force -ErrorAction Stop}
        }
        if(Test-ProcessSnapshotMatch $LauncherSnapshot){Stop-Process -Id ([int]$LauncherSnapshot.pid) -Force -ErrorAction Stop}
        $wait=[Diagnostics.Stopwatch]::StartNew()
        while($wait.ElapsedMilliseconds -lt 2000){
            $remaining=@($candidates.Values|Where-Object{Test-ProcessSnapshotMatch $_.snapshot})
            if(-not(Test-ProcessSnapshotMatch $LauncherSnapshot) -and $remaining.Count -eq 0){break}
            Start-Sleep -Milliseconds 50
        }
        $remaining=@($candidates.Values|Where-Object{Test-ProcessSnapshotMatch $_.snapshot})
        if(Test-ProcessSnapshotMatch $LauncherSnapshot){$remaining+=([pscustomobject]@{snapshot=$LauncherSnapshot;depth=0})}
        return [pscustomobject]@{stopped=$remaining.Count -eq 0;remaining=$remaining.Count}
    }
    finally {
        foreach($path in @($StopPath,(Join-Path $StateRoot "runtime.ready.json"),(Join-Path $StateRoot "runtime.identity.json"))){if(-not [string]::IsNullOrWhiteSpace([string]$path)){Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue}}
    }
}
function ConvertFrom-JCodeMunchMcpResponse {
    param([Parameter(Mandatory=$true)][object]$Response)
    $body=[string]$Response.Content
    $candidates=[Collections.Generic.List[string]]::new()
    if($body.TrimStart().StartsWith("{")){$candidates.Add($body)}
    foreach($line in ($body -split "`r?`n")){
        $match=[regex]::Match($line,'^data:\s*(\{.*\})\s*$')
        if($match.Success){$candidates.Add($match.Groups[1].Value)}
    }
    foreach($candidate in $candidates){
        try{$message=$candidate|ConvertFrom-Json -ErrorAction Stop}catch{continue}
        if($null -ne $message.PSObject.Properties["result"] -or $null -ne $message.PSObject.Properties["error"]){return $message}
    }
    return $null
}
function Probe-Runtime {
    param([int]$Port)
    $probeStage="token"
    $token=Get-JCodeMunchSecret -StateRoot $StateRoot
    if([string]::IsNullOrWhiteSpace($token)){Write-Diagnostic -Event "runtime_probe_failed" -Level "debug" -Details @{stage=$probeStage;reason="missing_token";port=$Port};return $null}
    $endpoint="http://127.0.0.1:$Port/mcp"
    $headers=@{Authorization="Bearer $token";Accept="application/json, text/event-stream"}
    $sessionId=$null
    try{
        $probeStage="initialize_request"
        $initialize=@{jsonrpc="2.0";id=1;method="initialize";params=@{protocolVersion="2025-03-26";capabilities=@{};clientInfo=@{name="jcodemunch-lifecycle-probe";version="1"}}}|ConvertTo-Json -Depth 8 -Compress
        $initializeResponse=Invoke-WebRequest -Method Post -Uri $endpoint -Headers $headers -ContentType "application/json" -Body $initialize -TimeoutSec 3
        $probeStage="initialize_response"
        $initializeMessage=ConvertFrom-JCodeMunchMcpResponse $initializeResponse
        if($null -eq $initializeMessage -or $null -eq $initializeMessage.result -or [string]$initializeMessage.result.serverInfo.name -ne "jcodemunch-mcp"){throw "invalid_initialize_response"}
        $sessionId=[string]$initializeResponse.Headers["Mcp-Session-Id"]
        if([string]::IsNullOrWhiteSpace($sessionId)){throw "missing_session_id"}
        $headers["Mcp-Session-Id"]=$sessionId
        $probeStage="initialized_notification"
        $initialized=@{jsonrpc="2.0";method="notifications/initialized"}|ConvertTo-Json -Compress
        $initializedResponse=Invoke-WebRequest -Method Post -Uri $endpoint -Headers $headers -ContentType "application/json" -Body $initialized -TimeoutSec 3
        if([int]$initializedResponse.StatusCode -ne 202){throw "invalid_initialized_status"}
        $probeStage="identity_request"
        $read=@{jsonrpc="2.0";id=2;method="resources/read";params=@{uri="munch://runtime/identity"}}|ConvertTo-Json -Depth 5 -Compress
        $readResponse=Invoke-WebRequest -Method Post -Uri $endpoint -Headers $headers -ContentType "application/json" -Body $read -TimeoutSec 3
        $probeStage="identity_response"
        $readMessage=ConvertFrom-JCodeMunchMcpResponse $readResponse
        $identityText=[string]$readMessage.result.contents[0].text
        if([string]::IsNullOrWhiteSpace($identityText)){throw "missing_identity"}
        $identity=$identityText|ConvertFrom-Json -ErrorAction Stop
        $manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot "versions.json") -Raw|ConvertFrom-Json
        if([string]$identity.schema -ne "munch.runtime.identity/v1" -or [string]$identity.product -ne "jcodemunch-mcp" -or [string]$identity.version -ne [string]$manifest.jcodemunch.version -or [string]$identity.transport -ne "streamable-http" -or [string]::IsNullOrWhiteSpace([string]$identity.instance_id) -or [string]::IsNullOrWhiteSpace([string]$identity.launch_id)){throw "identity_contract_mismatch"}
        $probeStage="process_identity"
        $runtimePid=0
        if(-not[int]::TryParse([string]$identity.pid,[ref]$runtimePid)-or$runtimePid-lt1){throw "invalid_runtime_pid"}
        $snapshot=Get-ProcessSnapshot $runtimePid
        if($null -eq $snapshot){throw "missing_process_snapshot"}
        $probeStage="listener_identity"
        $listener=@(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue|Where-Object{$_.LocalAddress -eq "127.0.0.1" -and [int]$_.OwningProcess -eq $runtimePid})
        if($listener.Count -ne 1){throw "listener_identity_mismatch"}
        $commandMaterial=if(-not[string]::IsNullOrWhiteSpace([string]$snapshot.command_line)){[string]$snapshot.command_line}else{[string]$snapshot.executable_path}
        $fingerprint="sha256:"+[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($commandMaterial))).ToLowerInvariant()
        return [pscustomobject]@{server_name="jcodemunch";runtime_version=[string]$identity.version;transport="streamable-http";bind_host="127.0.0.1";port=$Port;pid=$runtimePid;launch_id=[string]$identity.launch_id;runtime_identity=[string]$identity.instance_id;command_fingerprint=$fingerprint;runtime_process_start=$identity.process_start}
    }catch{Write-Diagnostic -Event "runtime_probe_failed" -Level "debug" -Details @{stage=$probeStage;reason=$_.Exception.Message;port=$Port};return $null}
    finally{
        if(-not[string]::IsNullOrWhiteSpace($sessionId)){
            try{Invoke-WebRequest -Method Delete -Uri $endpoint -Headers $headers -TimeoutSec 2|Out-Null}catch{}
        }
    }
}
function Get-OwnershipValidation {
    param([psobject]$Owner,[switch]$AllowUnconfiguredPort)
    $failed=[Collections.Generic.List[string]]::new()
    if($null -eq $Owner){
        $failed.Add("owner_record")
        return [pscustomobject]@{valid=$false;failed_checks=@($failed);message="No recorded owner tuple exists."}
    }
    foreach($required in @("pid","process_start_time","executable_sha256","runtime_version","port","runtime_identity","launch_id","command_fingerprint","server_name","transport","bind_host")){
        if($null -eq $Owner.PSObject.Properties[$required] -or [string]::IsNullOrWhiteSpace([string]$Owner.$required)){$failed.Add($required)}
    }
    $hasExecutable=$null -ne $Owner.PSObject.Properties["executable_path"] -and -not [string]::IsNullOrWhiteSpace([string]$Owner.executable_path)
    $hasCommand=$null -ne $Owner.PSObject.Properties["command_line"] -and -not [string]::IsNullOrWhiteSpace([string]$Owner.command_line)
    if(-not $hasExecutable -and -not $hasCommand){$failed.Add("executable_or_command")}
    $ownerPid=0;$ownerPort=0
    if(-not [int]::TryParse([string]$Owner.pid,[ref]$ownerPid) -or $ownerPid -lt 1){$failed.Add("pid")}
    if(-not [int]::TryParse([string]$Owner.port,[ref]$ownerPort) -or $ownerPort -lt 1 -or $ownerPort -gt 65535){$failed.Add("port")}
    $snapshot=$null
    if($failed -notcontains "pid"){$snapshot=Get-ProcessSnapshot $ownerPid;if($null -eq $snapshot){$failed.Add("process_snapshot")}}
    if($null -ne $snapshot){
        if([string]$snapshot.process_start_time -ne [string]$Owner.process_start_time){$failed.Add("process_start_time")}
        if($hasExecutable -and [string]$snapshot.executable_path -ne [string]$Owner.executable_path){$failed.Add("executable_path")}
        if($hasCommand -and [string]$snapshot.command_line -ne [string]$Owner.command_line){$failed.Add("command_line")}
        if($hasExecutable -and (Test-Path -LiteralPath $snapshot.executable_path -PathType Leaf)){
            try {
                $currentExecutableHash=(Get-FileHash -LiteralPath $snapshot.executable_path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
                if($currentExecutableHash -ne [string]$Owner.executable_sha256){$failed.Add("executable_sha256")}
            } catch {$failed.Add("executable_sha256")}
        } elseif($hasExecutable){$failed.Add("executable_sha256")}
    }
    if(-not$AllowUnconfiguredPort){
        $configured=Read-State "port"
        if($null -eq $configured -or [int]$configured.port -ne $ownerPort){$failed.Add("configured_port")}
    }
    $identity=$null
    if($failed -notcontains "port"){$identity=Probe-Runtime $ownerPort;if($null -eq $identity){$failed.Add("runtime_identity")}}
    if($null -ne $identity){
        if([int]$identity.pid -ne $ownerPid){$failed.Add("pid")}
        foreach($field in @("runtime_version","port","runtime_identity","launch_id","command_fingerprint","server_name","transport","bind_host")){
            if([string]$identity.$field -ne [string]$Owner.$field){$failed.Add($field)}
        }
    }
    $unique=@($failed|Sort-Object -Unique)
    $message=if($unique.Count -eq 0){"Complete ownership tuple matched."}else{"Ownership proof failed: $([string]::Join(', ', $unique))."}
    return [pscustomobject]@{valid=$unique.Count -eq 0;failed_checks=$unique;message=$message}
}
function Test-Owned {
    param([psobject]$Owner,[switch]$AllowUnconfiguredPort)
    return [bool](Get-OwnershipValidation $Owner -AllowUnconfiguredPort:$AllowUnconfiguredPort).valid
}
function Assert-Owned {
    param(
        [Parameter(Mandatory=$true)][psobject]$Owner,
        [Parameter(Mandatory=$true)][string]$Operation
    )
    $validation=Get-OwnershipValidation $Owner
    if(-not $validation.valid){
        $details=@{process_action="none";action="inspect-and-reselect";failed_checks=@($validation.failed_checks)}
        if($null -ne $Owner.PSObject.Properties["port"]){$details.port=[int]$Owner.port}
        Throw-LifecycleError "ownership_mismatch" "Cannot ${Operation}: $($validation.message) No process action was taken; inspect the failed checks and explicitly reselect the port if required." $details
    }
    return $validation
}
function Test-LeaseLive {
    param([psobject]$Lease)
    if($null -eq $Lease -or $null -eq $Lease.PSObject.Properties["pid"] -or $null -eq $Lease.PSObject.Properties["process_start_time"] -or [string]::IsNullOrWhiteSpace([string]$Lease.process_start_time)){return $null}
    $leasePid=0
    if(-not [int]::TryParse([string]$Lease.pid,[ref]$leasePid) -or $leasePid -lt 1){return $null}
    try { $null=Get-Process -Id $leasePid -ErrorAction Stop } catch { return $false }
    $start=Get-ProcessStart $leasePid
    if($null -eq $start){return $null}
    return [string]$start -eq [string]$Lease.process_start_time
}
function Repair-StaleLeases {
    $leases=@(Get-Leases)
    if($leases.Count -eq 0){return @()}
    $kept=[Collections.Generic.List[object]]::new();$removed=0
    foreach($lease in $leases){
        $live=Test-LeaseLive $lease
        if($live -eq $false){$removed++;continue}
        $kept.Add($lease)
    }
    if($removed -gt 0){Save-Leases @($kept);Write-Diagnostic -Event "stale_leases_repaired" -Details @{removed=$removed;remaining=$kept.Count}}
    return @($kept)
}
function Stop-ProcessWithProof {
    param([Parameter(Mandatory=$true)][psobject]$Owner,[switch]$AllowUnconfiguredPort)
    if(-not(Test-Owned $Owner -AllowUnconfiguredPort:$AllowUnconfiguredPort)){return $false}
    Stop-Process -Id ([int]$Owner.pid) -Force -ErrorAction Stop
    return $true
}
function Get-PortRejection {
    param([int]$Port,[psobject]$Policy)
    if($Port -lt 1 -or $Port -gt 65535){return [pscustomobject]@{port=$Port;reason="invalid_port";detail="outside_1_65535"}};if($Port -lt 1024){return [pscustomobject]@{port=$Port;reason="privileged";detail="below_1024"}}
    if(@($Policy.candidate_ranges).Count -gt 0 -and @($Policy.candidate_ranges|Where-Object{$_.start -le $Port -and $_.end -ge $Port}).Count -eq 0){return [pscustomobject]@{port=$Port;reason="outside_candidate_range"}}
    foreach($rule in @(@($Policy.reserved_ranges)+@($Policy.denylisted_ranges)+@($Policy.dynamic_ranges)+@($Policy.excluded_ranges))){if([int]$rule.start -le $Port -and [int]$rule.end -ge $Port){$reason=if([string]$rule.source -like "*denylist*"){"denylisted"}elseif([string]$rule.source -like "*reserved*"){"reserved"}elseif([string]$rule.source -like "*dynamic*"){"dynamic_range"}else{"excluded_range"};return [pscustomobject]@{port=$Port;reason=$reason;range=[pscustomobject]@{start=[int]$rule.start;end=[int]$rule.end;source=[string]$rule.source}}}}
    $probe=Test-PortAvailable $Port;if(-not $probe.available){$family=if($null -ne $probe.PSObject.Properties["address_family"]){$probe.address_family}else{$null};$occupants=if($null -ne $probe.PSObject.Properties["occupants"]){$probe.occupants}else{$null};return [pscustomobject]@{port=$Port;reason=[string]$probe.reason;address_family=$family;occupants=$occupants}};return $null
}
function Stop-PortProbe {
    param([AllowNull()][psobject]$Probe)
    if($null -eq $Probe){return}
    try {
        if($null -ne $Probe.stop_path){New-Item -ItemType File -Path $Probe.stop_path -Force|Out-Null}
        $clock=[Diagnostics.Stopwatch]::StartNew()
        while($clock.ElapsedMilliseconds -lt 3000 -and $null -ne (Get-ProcessStart ([int]$Probe.pid))){Start-Sleep -Milliseconds 50}
        if($null -ne (Get-ProcessStart ([int]$Probe.pid))){
            $validation=Get-OwnershipValidation $Probe -AllowUnconfiguredPort
            if(-not$validation.valid){Throw-LifecycleError "probe_ownership_mismatch" "The temporary port probe no longer matches its complete ownership tuple." @{port=[int]$Probe.port;process_action="none";failed_checks=@($validation.failed_checks)}}
            Stop-Process -Id ([int]$Probe.pid) -Force -ErrorAction Stop
        }
    }
    finally {
        foreach($path in @($Probe.ready_path,$Probe.identity_path,$Probe.stop_path)){if($null -ne $path){Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue}}
    }
}
function Invoke-PortProbe {
    param([int]$Port)
    $probe=$null
    try {
        $probe=Start-Runtime $Port
        $identity=Probe-Runtime $Port
        if($null -eq $identity -or [int]$identity.pid -ne [int]$probe.pid -or [string]$identity.runtime_identity -ne [string]$probe.runtime_identity){return $false}
        return $true
    }
    catch {
        $source=$_.Exception;$code=$null
        while($null -eq $code -and $null -ne $source){if($null -ne $source.Data["code"]){$code=[string]$source.Data["code"]};$source=$source.InnerException}
        Write-Diagnostic -Event "port_probe_failed" -Level "debug" -Details @{port=$Port;code=$code;reason=$_.Exception.Message}
        if($code -in @("runtime_integrity","runtime_unavailable","probe_ownership_mismatch","bootstrap_cleanup_failed")){throw}
        return $false
    }
    finally {Stop-PortProbe $probe}
}
function Select-Port {
    $policy=Get-PortRangePolicy;$candidates=[Collections.Generic.List[int]]::new();foreach($port in @(Parse-Ports $CandidatePorts)){$candidates.Add([int]$port)};foreach($range in @($policy.candidate_ranges)){for($port=[int]$range.start;$port -le [int]$range.end;$port++){$candidates.Add($port)}};if($candidates.Count -eq 0){foreach($port in 45100..45199){$candidates.Add($port)}};$unique=@($candidates|Sort-Object -Unique);$rejections=[Collections.Generic.List[object]]::new();foreach($port in $unique){$rejection=Get-PortRejection -Port ([int]$port) -Policy $policy;if($null -eq $rejection){if(Invoke-PortProbe ([int]$port)){return [pscustomobject]@{port=[int]$port;range_policy=$policy;probe_verified=$true}};$rejections.Add([pscustomobject]@{port=[int]$port;reason="probe_failed";detail="runtime_identity_probe_failed"});continue};$rejections.Add($rejection)};$details=@{rejections=@($rejections);range_policy=$policy};if($unique.Count -eq 1){$details.port=[int]$unique[0]};Throw-LifecycleError "port_unavailable" "No candidate loopback port passed the fail-closed safety checks and runtime identity probe." $details
}
function Get-ConfigBytesHash {
    param([byte[]]$Bytes)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}
function New-ConfigBackupEntry {
    param([Parameter(Mandatory=$true)][string]$Path)
    $fullPath=[IO.Path]::GetFullPath($Path)
    if(Test-Path -LiteralPath $fullPath -PathType Container){Throw-LifecycleError "config_target_is_directory" "Configuration target is a directory, not a file: $fullPath" @{path=$fullPath}}
    if(Test-Path -LiteralPath $fullPath -PathType Leaf){
        $bytes=[IO.File]::ReadAllBytes($fullPath)
        $sddl=$null;$securityDescriptor=$null
        if([OperatingSystem]::IsWindows()){
            $securityDescriptor=Get-JCodeMunchSecurityDescriptor -Path $fullPath
            try{$sddl=(Get-Acl -LiteralPath $fullPath -ErrorAction Stop).Sddl}catch{}
        }
        return [pscustomobject]@{path=$fullPath;existed=$true;content_base64=[Convert]::ToBase64String($bytes);content_sha256=(Get-ConfigBytesHash $bytes);acl_sddl=$sddl;security_descriptor_base64=$securityDescriptor;captured_at=[DateTime]::UtcNow.ToString("o")}
    }
    return [pscustomobject]@{path=$fullPath;existed=$false;content_base64=$null;content_sha256=$null;captured_at=[DateTime]::UtcNow.ToString("o")}
}
function New-TokenEnvironmentBackup {
    param([Parameter(Mandatory=$true)][ValidateSet("Process","User")][string]$Scope)
    $value=[Environment]::GetEnvironmentVariable("JCODEMUNCH_HTTP_TOKEN",$Scope)
    $existed=$null -ne $value
    $encoded=if($existed){[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([string]$value))}else{$null}
    return [pscustomobject]@{name="JCODEMUNCH_HTTP_TOKEN";scope=$Scope;existed=$existed;value_base64=$encoded;captured_at=[DateTime]::UtcNow.ToString("o")}
}
function Set-TokenEnvironmentValue {
    param([AllowNull()][string]$Value,[Parameter(Mandatory=$true)][ValidateSet("Process","User")][string]$Scope)
    [Environment]::SetEnvironmentVariable("JCODEMUNCH_HTTP_TOKEN",$Value,$Scope)
    if($Scope -eq "User" -and [OperatingSystem]::IsWindows()){
        if($null -eq ("JCodeMunch.NativeEnvironment" -as [type])){
            Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace JCodeMunch {
    public static class NativeEnvironment {
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr SendMessageTimeout(
            IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
            uint flags, uint timeout, out UIntPtr result);
    }
}
"@
        }
        $broadcast=[IntPtr]0xffff;$result=[UIntPtr]::Zero
        [void][JCodeMunch.NativeEnvironment]::SendMessageTimeout($broadcast,0x001A,[UIntPtr]::Zero,"Environment",2,5000,[ref]$result)
    }
}
function Write-AtomicConfigBytes {
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][byte[]]$Bytes,[AllowNull()][string]$AclSddl,[AllowNull()][string]$SecurityDescriptorBase64)
    $destination=[IO.Path]::GetFullPath($Path);$parent=Split-Path -Parent $destination
    if(-not(Test-Path -LiteralPath $parent -PathType Container)){New-Item -ItemType Directory -Path $parent -Force|Out-Null}
    $temp=Join-Path $parent (".{0}.{1}.rollback.tmp" -f [IO.Path]::GetFileName($destination),[Guid]::NewGuid().ToString("N"))
    $replacementBackup=Join-Path $parent (".{0}.{1}.rollback.bak" -f [IO.Path]::GetFileName($destination),[Guid]::NewGuid().ToString("N"))
    try {
        [IO.File]::WriteAllBytes($temp,$Bytes)
        $preserved=$false
        if(Test-Path -LiteralPath $destination -PathType Leaf){
            try{[IO.File]::Replace($temp,$destination,$replacementBackup,$true);$preserved=$true;Remove-Item -LiteralPath $replacementBackup -Force -ErrorAction SilentlyContinue}catch [PlatformNotSupportedException]{[IO.File]::Move($temp,$destination,$true)}
        }else{[IO.File]::Move($temp,$destination)}
        if(-not[string]::IsNullOrWhiteSpace($SecurityDescriptorBase64)-and[OperatingSystem]::IsWindows()){
            Set-JCodeMunchSecurityDescriptor -Path $destination -DescriptorBase64 $SecurityDescriptorBase64
            if(-not(Test-JCodeMunchSecurityDescriptorEquivalent -ExpectedBase64 $SecurityDescriptorBase64 -ActualBase64 (Get-JCodeMunchSecurityDescriptor -Path $destination))){throw "Restored security descriptor verification failed."}
        }
    } finally {if(Test-Path -LiteralPath $temp -PathType Leaf){Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue};if(Test-Path -LiteralPath $replacementBackup -PathType Leaf){Remove-Item -LiteralPath $replacementBackup -Force -ErrorAction SilentlyContinue}}
}
function Restore-ConfigBackups {
    param([Parameter(Mandatory=$true)][psobject]$Backups)
    $restored=0;$deleted=0;$failures=[Collections.Generic.List[object]]::new()
    foreach($entry in @($Backups.entries)){
        try {
            $existed=if($null -eq $entry.PSObject.Properties["existed"]){$true}else{[bool]$entry.existed}
            if($existed){
                $bytes=[Convert]::FromBase64String([string]$entry.content_base64)
                if($null -ne $entry.PSObject.Properties["content_sha256"] -and -not[string]::IsNullOrWhiteSpace([string]$entry.content_sha256)){
                    $actual=Get-ConfigBytesHash $bytes
                    if($actual -ne [string]$entry.content_sha256){throw "Backup content integrity mismatch."}
                }
                $aclSddl=if($null-ne$entry.PSObject.Properties["acl_sddl"]){[string]$entry.acl_sddl}else{$null}
                $securityDescriptor=if($null-ne$entry.PSObject.Properties["security_descriptor_base64"]){[string]$entry.security_descriptor_base64}else{$null}
                Write-AtomicConfigBytes -Path ([string]$entry.path) -Bytes $bytes -AclSddl $aclSddl -SecurityDescriptorBase64 $securityDescriptor;$restored++
            }elseif(Test-Path -LiteralPath ([string]$entry.path) -PathType Leaf){
                Remove-Item -LiteralPath ([string]$entry.path) -Force -ErrorAction Stop;$deleted++
            }
        } catch {$failures.Add([pscustomobject]@{path=[string]$entry.path;message=$_.Exception.Message})}
    }
    $environmentRestored=0
    if($null -ne $Backups.PSObject.Properties["environment"]){
        foreach($entry in @($Backups.environment)){
            try {
                $scope=[string]$entry.scope
                if($scope -notin @("Process","User")){throw "Unsupported environment backup scope '$scope'."}
                $value=if([bool]$entry.existed){[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$entry.value_base64))}else{$null}
                Set-TokenEnvironmentValue -Value $value -Scope $scope
                $environmentRestored++
            } catch {$failures.Add([pscustomobject]@{path="env:$([string]$entry.name)";message=$_.Exception.Message})}
        }
    }
    return [pscustomobject]@{restored=$restored;deleted=$deleted;environment_restored=$environmentRestored;failed=$failures.Count;failures=@($failures)}
}
function Read-JsonConfigMap {
    param([Parameter(Mandatory=$true)][string]$Path,[switch]$AllowMissing)
    if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){
        if($AllowMissing){return [ordered]@{}}
        Throw-LifecycleError "config_missing" "Required JSON configuration does not exist: $Path" @{path=$Path}
    }
    try {$document=Get-Content -LiteralPath $Path -Raw -Encoding utf8|ConvertFrom-Json -AsHashtable}catch{Throw-LifecycleError "config_parse_failed" "Invalid JSON configuration at $Path`: $($_.Exception.Message)" @{path=$Path}}
    if($document -isnot [Collections.IDictionary]){Throw-LifecycleError "config_schema_unknown" "JSON configuration root must be an object: $Path" @{path=$Path}}
    return $document
}
function Get-OrAddConfigMap {
    param([Parameter(Mandatory=$true)][Collections.IDictionary]$Map,[Parameter(Mandatory=$true)][string]$Key,[string]$Path)
    if(-not $Map.Contains($Key)){$Map[$Key]=[ordered]@{}}
    if($Map[$Key] -isnot [Collections.IDictionary]){Throw-LifecycleError "config_schema_unknown" "Expected '$Key' to be an object in $Path." @{path=$Path;property=$Key}}
    return $Map[$Key]
}
function Test-TomlConfig {
    param([Parameter(Mandatory=$true)][string]$Path)
    $python=(Get-Command python -ErrorAction SilentlyContinue).Source
    if([string]::IsNullOrWhiteSpace($python)){Throw-LifecycleError "toml_validator_unavailable" "Python is required to validate Codex TOML safely."}
    & $python -c "import sys,tomllib; tomllib.load(open(sys.argv[1], 'rb'))" $Path 2>&1|Out-Null
    if($LASTEXITCODE -ne 0){Throw-LifecycleError "config_parse_failed" "Invalid TOML configuration at $Path." @{path=$Path}}
}
function Test-CodexJCodeMunchEntry {
    param([Parameter(Mandatory=$true)][string]$Path)
    if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){return $false}
    $python=(Get-Command python -ErrorAction SilentlyContinue).Source
    if([string]::IsNullOrWhiteSpace($python)){Throw-LifecycleError "toml_validator_unavailable" "Python is required to inspect Codex TOML safely."}
    $result=(& $python -c "import sys,tomllib; d=tomllib.load(open(sys.argv[1], 'rb')); m=d.get('mcp_servers', {}); print('true' if isinstance(m, dict) and 'jcodemunch' in m else 'false')" $Path 2>&1|Out-String).Trim()
    if($LASTEXITCODE-ne0-or$result-notin@("true","false")){Throw-LifecycleError "config_parse_failed" "Invalid TOML configuration at $Path." @{path=$Path}}
    return $result-eq"true"
}
function Get-MergedCodexConfigText {
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][string]$BridgePath,[Parameter(Mandatory=$true)][string]$ClientName)
    if(Test-Path -LiteralPath $Path -PathType Leaf){
        Test-TomlConfig $Path;$text=Get-Content -LiteralPath $Path -Raw -Encoding utf8
        if((Test-CodexJCodeMunchEntry $Path)-and$text-notmatch'(?m)^\s*\[\s*mcp_servers\.jcodemunch\s*\]\s*(?:#.*)?$'){
            Throw-LifecycleError "config_schema_unknown" "The existing Codex jcodemunch entry uses quoted, dotted, or inline TOML that cannot be rewritten losslessly." @{path=$Path;action="normalize-jcodemunch-table"}
        }
    }else{$text=""}
    $kept=[Collections.Generic.List[string]]::new();$skip=$false
    foreach($line in @($text -split "`r?`n")){
        if($line -match '^\s*\[{1,2}\s*([^\]]+?)\s*\]{1,2}\s*(?:#.*)?$'){
            $section=($Matches[1] -replace '\s','')
            $skip=$section -match '^mcp_servers\.jcodemunch(?:\.|$)'
        }
        if(-not $skip){$kept.Add($line)}
    }
    $base=(@($kept) -join "`n").TrimEnd()
    $bridgeJson=$BridgePath|ConvertTo-Json -Compress
    $clientJson=$ClientName|ConvertTo-Json -Compress
    $block=@"
[mcp_servers.jcodemunch]
command = "pwsh"
args = ["-NoProfile", "-NonInteractive", "-File", $bridgeJson, "-Client", $clientJson]
enabled = true
startup_timeout_sec = 60
tool_timeout_sec = 300
"@
    if([string]::IsNullOrWhiteSpace($base)){return $block.TrimStart() + "`n"}
    return $base + "`n`n" + $block.Trim() + "`n"
}
function Invoke-TransactionalTextWrite {
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][string]$Text,[Parameter(Mandatory=$true)][ref]$WriteCount,[switch]$PreserveAcl)
    Write-JCodeMunchAtomicText -Path $Path -Text $Text -PreserveAcl:$PreserveAcl
    $WriteCount.Value++
    if($FailureInjectionAfterWrites -gt 0 -and $WriteCount.Value -eq $FailureInjectionAfterWrites){throw "Injected configuration transaction failure after $($WriteCount.Value) writes."}
}
function Remove-JCodeMunchClaudeHooks {
    param([Parameter(Mandatory=$true)][Collections.IDictionary]$Settings)
    if(-not $Settings.Contains("hooks") -or $Settings["hooks"] -isnot [Collections.IDictionary]){return $false}
    $changed=$false;$hooks=$Settings["hooks"]
    foreach($event in @($hooks.Keys)){
        [object[]]$groups=@($hooks[$event])
        [object[]]$kept=@($groups|Where-Object{
            $serialized=$_|ConvertTo-Json -Depth 30 -Compress
            $serialized -notmatch '(?i)jcodemunch(?:_lifecycle|\\|/).*claude-session\.ps1'
        })
        if($kept.Count -ne $groups.Count){$changed=$true;$hooks[$event]=@($kept)}
    }
    return $changed
}
function Invoke-ClientConfigTransaction {
    param([Parameter(Mandatory=$true)][int]$Port,[AllowNull()][string]$Token)
    foreach($required in @(@{name="CodexConfig";value=$CodexConfig},@{name="ClaudeConfig";value=$ClaudeConfig},@{name="ClaudeSettingsConfig";value=$ClaudeSettingsConfig},@{name="PiConfig";value=$PiConfig},@{name="PiSettingsConfig";value=$PiSettingsConfig},@{name="IntegrationRoot";value=$IntegrationRoot})){
        if([string]::IsNullOrWhiteSpace([string]$required.value)){Throw-LifecycleError "config_target_missing" "$($required.name) is required when ApplyClientConfigs is set." @{parameter=$required.name}}
    }
    if($FailureInjectionAfterWrites -lt 0){Throw-LifecycleError "invalid_failure_injection" "FailureInjectionAfterWrites cannot be negative."}
    $integration=[IO.Path]::GetFullPath($IntegrationRoot);$supportSource=$PSScriptRoot
    $assetPaths=if($SourceAssetPaths.Count-gt0){@($SourceAssetPaths|ForEach-Object{([string]$_)-split','}|Where-Object{-not[string]::IsNullOrWhiteSpace($_)})}else{
        $installedManifestPath=Join-Path $supportSource "integration-manifest.json"
        if(-not(Test-Path -LiteralPath $installedManifestPath -PathType Leaf)){Throw-LifecycleError "asset_manifest_missing" "A source asset list or installed integration manifest is required."}
        $installedManifest=Get-Content -LiteralPath $installedManifestPath -Raw|ConvertFrom-Json;@($installedManifest.files|ForEach-Object{[string]$_.path})
    }
    $sourceTargets=[Collections.Generic.List[object]]::new()
    $seenAssets=@{}
    foreach($relativeInput in @($assetPaths|Sort-Object -Unique)){
        $relative=([string]$relativeInput).Replace('/',[IO.Path]::DirectorySeparatorChar)
        if([string]::IsNullOrWhiteSpace($relative)-or[IO.Path]::IsPathRooted($relative)-or$relative-match'(^|[\\/])\.\.([\\/]|$)'-or$seenAssets.ContainsKey($relative)){Throw-LifecycleError "asset_path_unsafe" "The integration asset list contains an unsafe or duplicate path."}
        $source=[IO.Path]::GetFullPath((Join-Path $supportSource $relative));$sourcePrefix=[IO.Path]::GetFullPath($supportSource).TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
        if(-not$source.StartsWith($sourcePrefix,[StringComparison]::OrdinalIgnoreCase)-or-not(Test-Path -LiteralPath $source -PathType Leaf)){Throw-LifecycleError "asset_missing" "A listed integration source asset is missing." @{path=$relative}}
        $seenAssets[$relative]=$true;$sourceTargets.Add([pscustomobject]@{source=$source;target=(Join-Path $integration $relative)})
    }
    $expectedTargets=@{};foreach($item in $sourceTargets){$expectedTargets[[IO.Path]::GetFullPath([string]$item.target)]=$true}
    $rootIntegrationManifest=[IO.Path]::GetFullPath((Join-Path $integration "integration-manifest.json"))
    $staleTargets=@();if(Test-Path -LiteralPath $integration -PathType Container){$staleTargets=@(Get-ChildItem -LiteralPath $integration -File -Recurse|Where-Object{-not$expectedTargets.ContainsKey($_.FullName)-and-not[string]::Equals($_.FullName,$rootIntegrationManifest,[StringComparison]::OrdinalIgnoreCase)}|ForEach-Object{$_.FullName})}
    $allTargets=[Collections.Generic.List[string]]::new();foreach($path in @($CodexConfig,$ClaudeConfig,$ClaudeSettingsConfig,$PiConfig,$PiSettingsConfig)){$allTargets.Add([IO.Path]::GetFullPath($path))};foreach($item in $sourceTargets){$allTargets.Add([IO.Path]::GetFullPath([string]$item.target))};foreach($path in $staleTargets){$allTargets.Add($path)}
    $existingBackups=Read-State "backups"
    $canReuse=$null -ne $existingBackups -and [string]$existingBackups.status -eq "applied" -and @($existingBackups.entries).Count -gt 0
    if($canReuse){
        foreach($path in @($allTargets|Sort-Object -Unique)){
            $fullTarget=[IO.Path]::GetFullPath($path)
            $matched=@($existingBackups.entries|Where-Object{[string]::Equals([string]$_.path,$fullTarget,[StringComparison]::OrdinalIgnoreCase)})
            if($matched.Count -ne 1){$canReuse=$false;break}
        }
    }
    $transactionEntries=[Collections.Generic.List[object]]::new();foreach($path in @($allTargets|Sort-Object -Unique)){$transactionEntries.Add((New-ConfigBackupEntry $path))}
    $transactionBackup=[pscustomobject]@{schema_version=1;status="prepared";entries=@($transactionEntries);environment=@();captured_at=[DateTime]::UtcNow.ToString("o")}
    if($canReuse){$backupDocument=$existingBackups}else{
        $backupDocument=[pscustomobject]@{schema_version=1;transaction_id=[Guid]::NewGuid().ToString("N");status="prepared";entries=@($transactionBackup.entries);environment=@();captured_at=[string]$transactionBackup.captured_at}
        Write-State "backups" $backupDocument
    }
    $writeCount=0
    try {
        foreach($item in $sourceTargets){Invoke-TransactionalTextWrite -Path ([string]$item.target) -Text (Get-Content -LiteralPath ([string]$item.source) -Raw -Encoding utf8) -WriteCount ([ref]$writeCount)}
        foreach($stale in $staleTargets){Remove-Item -LiteralPath $stale -Force -ErrorAction Stop}
        $bridgePath=Join-Path $integration "jcodemunch-bridge.ps1"
        Invoke-TransactionalTextWrite -Path $CodexConfig -Text (Get-MergedCodexConfigText -Path $CodexConfig -BridgePath $bridgePath -ClientName "codex") -WriteCount ([ref]$writeCount) -PreserveAcl;Test-TomlConfig $CodexConfig
        $claude=Read-JsonConfigMap -Path $ClaudeConfig -AllowMissing;$claudeServers=Get-OrAddConfigMap -Map $claude -Key "mcpServers" -Path $ClaudeConfig;$claudeServers["jcodemunch"]=[ordered]@{type="stdio";command="pwsh";args=@("-NoProfile","-NonInteractive","-File",$bridgePath,"-Client","claude")}
        Invoke-TransactionalTextWrite -Path $ClaudeConfig -Text ($claude|ConvertTo-Json -Depth 50) -WriteCount ([ref]$writeCount) -PreserveAcl;[void](Read-JsonConfigMap -Path $ClaudeConfig)
        $claudeSettings=Read-JsonConfigMap -Path $ClaudeSettingsConfig -AllowMissing
        if(Remove-JCodeMunchClaudeHooks -Settings $claudeSettings){Invoke-TransactionalTextWrite -Path $ClaudeSettingsConfig -Text ($claudeSettings|ConvertTo-Json -Depth 50) -WriteCount ([ref]$writeCount) -PreserveAcl;[void](Read-JsonConfigMap -Path $ClaudeSettingsConfig)}
        $manifest=Get-Content -LiteralPath (Join-Path $supportSource "versions.json") -Raw -Encoding utf8|ConvertFrom-Json
        $piArtifact=Join-Path (Join-Path (Join-Path (Join-Path $StateRoot "runtimes") ([string]$manifest.pi_adapter.package)) ([string]$manifest.pi_adapter.version)) ("$($manifest.pi_adapter.package)-$($manifest.pi_adapter.version).tgz")
        if(-not(Test-Path -LiteralPath $piArtifact -PathType Leaf)){Throw-LifecycleError "pi_adapter_missing" "The verified local Pi adapter artifact is missing." @{path=$piArtifact}}
        $expectedPiIntegrity=([string]$manifest.pi_adapter.integrity)-replace'^sha512-',''
        if([Environment]::GetEnvironmentVariable("JCODEMUNCH_LIFECYCLE_TEST_MODE")-eq"1"){
            $testIntegrity=[Environment]::GetEnvironmentVariable("JCODEMUNCH_TEST_PI_INTEGRITY")
            if(-not[string]::IsNullOrWhiteSpace($testIntegrity)){$expectedPiIntegrity=$testIntegrity-replace'^sha512-',''}
        }
        $actualPiIntegrity=[Convert]::ToBase64String([Security.Cryptography.SHA512]::HashData([IO.File]::ReadAllBytes($piArtifact)))
        if($actualPiIntegrity-ne$expectedPiIntegrity){Throw-LifecycleError "pi_adapter_integrity" "The local Pi adapter artifact failed integrity verification." @{path=$piArtifact}}
        $piSpec="file:$(([IO.Path]::GetFullPath($piArtifact)).Replace('\','/'))"
        $piSettings=Read-JsonConfigMap -Path $PiSettingsConfig -AllowMissing
        [string[]]$packages=if($piSettings.Contains("packages")){@($piSettings["packages"])}else{@()}
        $packages=@($packages|Where-Object{
            -not [string]::IsNullOrWhiteSpace([string]$_) -and
            [string]$_ -ne $piSpec -and
            [string]$_ -notmatch '(?i)^(?:npm:)?pi-mcp-extension@' -and
            [string]$_ -notmatch '(?i)^file:.*pi-mcp-extension-.*\.tgz$' -and
            [string]$_ -notmatch '(?i)jcodemunch_lifecycle[\\/]pi'
        })
        $packages+=@($piSpec);$piSettings["packages"]=@($packages)
        [string[]]$extensions=if($piSettings.Contains("extensions")){@($piSettings["extensions"])}else{@()}
        $piSettings["extensions"]=@($extensions|Where-Object{
            -not [string]::IsNullOrWhiteSpace([string]$_) -and
            [string]$_ -notmatch '(?i)jcodemunch-lifecycle\.ts$'
        })
        Invoke-TransactionalTextWrite -Path $PiSettingsConfig -Text ($piSettings|ConvertTo-Json -Depth 50) -WriteCount ([ref]$writeCount) -PreserveAcl;[void](Read-JsonConfigMap -Path $PiSettingsConfig)
        $pi=Read-JsonConfigMap -Path $PiConfig -AllowMissing;$piServers=Get-OrAddConfigMap -Map $pi -Key "mcpServers" -Path $PiConfig;$piServers["jcodemunch"]=[ordered]@{transport="stdio";command="pwsh";args=@("-NoProfile","-NonInteractive","-File",$bridgePath,"-Client","pi");lifecycle="eager"}
        Invoke-TransactionalTextWrite -Path $PiConfig -Text ($pi|ConvertTo-Json -Depth 50) -WriteCount ([ref]$writeCount) -PreserveAcl;[void](Read-JsonConfigMap -Path $PiConfig)
        $backupDocument.status="applied";Write-State "backups" $backupDocument
        return [pscustomobject]@{applied=$true;writes=$writeCount;integration_root=$integration;bridge=$bridgePath;transport="stdio";pi_adapter=$piSpec;token_storage="owner_only_file"}
    } catch {
        $cause=$_.Exception.Message;$rollback=Restore-ConfigBackups $transactionBackup
        if($rollback.failed -gt 0){Throw-LifecycleError "config_rollback_failed" "Client configuration failed and rollback was incomplete." @{rollback=$rollback}}
        Throw-LifecycleError "config_transaction_failed" "Client configuration failed and was rolled back: $cause" @{rollback=$rollback}
    }
}
function Configure-Lifecycle {
    $policy=Get-PortRangePolicy;$current=Read-State "port";$owner=Read-State "owner";$existingBackups=Read-State "backups";
    if($null -ne $owner){[void](Assert-Owned $owner "reuse or replace the recorded jCodeMunch runtime")}
    $hasAppliedClientConfiguration=$null -ne $existingBackups -and [string]$existingBackups.status -eq "applied" -and @($existingBackups.entries).Count -gt 0
    if($ReselectPort -and $hasAppliedClientConfiguration -and -not $ApplyClientConfigs){
        Throw-LifecycleError "reselection_requires_client_transaction" "A configured client cutover exists; port reselection must regenerate every client configuration in the same transaction." @{action="reselect-with-client-configs"}
    }
    if($ReselectPort){
        if($null -ne $owner -and (Test-Owned $owner)){Throw-LifecycleError "reselection_requires_idle" "Stop the validated jCodeMunch singleton and restart clients before selecting a new port." @{action="stop-and-reconfigure";port=[int]$owner.port}}
        $selection=Select-Port
    }elseif($null -ne $current){
        $rejection=Get-PortRejection -Port ([int]$current.port) -Policy $policy;
        if($null -ne $owner -and (Test-Owned $owner)){$selection=[pscustomobject]@{port=[int]$current.port;range_policy=$policy;probe_verified=$true}}
        elseif($null -ne $rejection){Throw-LifecycleError "persisted_port_unsafe" "The persisted jCodeMunch port failed current safety checks; run configure --reselect-port and restart clients." @{action="reselect-port";port=[int]$current.port;rejection=$rejection;range_policy=$policy}}
        elseif(Invoke-PortProbe ([int]$current.port)){$selection=[pscustomobject]@{port=[int]$current.port;range_policy=$policy;probe_verified=$true}}
        else{Throw-LifecycleError "persisted_port_probe_failed" "The persisted jCodeMunch port did not pass the runtime identity probe; run configure --reselect-port and restart clients." @{action="reselect-port";port=[int]$current.port;range_policy=$policy}}
    }else{$selection=Select-Port};$port=[int]$selection.port
    $portStatePath=Get-JCodeMunchStatePath -Name port -StateRoot $StateRoot
    $portBackup=[pscustomobject]@{entries=@((New-ConfigBackupEntry $portStatePath));environment=@()}
    Write-State "port" ([pscustomobject]@{schema_version=1;port=$port;host="127.0.0.1";endpoint="http://127.0.0.1:$port";server_name="jcodemunch";transport="streamable-http";range_policy=$selection.range_policy;configured_at=[DateTime]::UtcNow.ToString("o")})
    $token=New-JCodeMunchSecret -StateRoot $StateRoot -LockHeld
    $clientConfiguration=$null
    if($ApplyClientConfigs){
        try {
            $clientConfiguration=Invoke-ClientConfigTransaction -Port $port -Token $token
        } catch {
            $portRollback=Restore-ConfigBackups $portBackup
            if($portRollback.failed -gt 0){Throw-LifecycleError "port_rollback_failed" "Client configuration failed and the previous port state could not be restored." @{rollback=$portRollback}}
            throw
        }
    }else{
        $entries=[Collections.Generic.List[object]]::new()
        foreach($path in @($CodexConfig,$ClaudeConfig,$ClaudeSettingsConfig,$PiConfig,$PiSettingsConfig)){
            if([string]::IsNullOrWhiteSpace($path)){continue}
            $entries.Add((New-ConfigBackupEntry $path))
        }
        if(-not $hasAppliedClientConfiguration -and $entries.Count -gt 0){
            Write-State "backups" ([pscustomobject]@{schema_version=1;status="prepared";entries=@($entries);captured_at=[DateTime]::UtcNow.ToString("o")})
        }
    }
    return [pscustomobject]@{endpoint=[pscustomobject]@{host="127.0.0.1";port=$port;url="http://127.0.0.1:$port";mcp_url="http://127.0.0.1:$port/mcp"};state_path=(Get-JCodeMunchStatePath -Name port -StateRoot $StateRoot);configured=$true;client_configuration=$clientConfiguration}
}
function Start-Runtime {
    param([int]$Port)
    $test=[Environment]::GetEnvironmentVariable("JCODEMUNCH_LIFECYCLE_TEST_MODE") -eq "1"
    $runtimeToken=Get-JCodeMunchSecret -StateRoot $StateRoot
    if([string]::IsNullOrWhiteSpace($runtimeToken)){$runtimeToken=New-JCodeMunchSecret -StateRoot $StateRoot -LockHeld}
    $runtimeLaunchId=[Guid]::NewGuid().ToString("N")
    $ready=Join-Path $StateRoot "runtime.ready.json"
    $idfile=Join-Path $StateRoot "runtime.identity.json"
    $stop=Join-Path $StateRoot "runtime.stop"
    Remove-Item -LiteralPath $ready,$idfile,$stop -Force -ErrorAction SilentlyContinue
    $runtimeEnvironment=@("JCODEMUNCH_HTTP_TOKEN","JCODEMUNCH_LAUNCH_ID","PYTHONPATH","PYTHONHOME","PYTHONSTARTUP","PYTHONNOUSERSITE","PYTHONDONTWRITEBYTECODE")
    $previousRuntimeEnvironment=@{};foreach($name in $runtimeEnvironment){$previousRuntimeEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,"Process")}
    try {
        foreach($name in @("PYTHONPATH","PYTHONHOME","PYTHONSTARTUP")){[Environment]::SetEnvironmentVariable($name,$null,"Process")}
        [Environment]::SetEnvironmentVariable("JCODEMUNCH_HTTP_TOKEN",$runtimeToken,"Process")
        [Environment]::SetEnvironmentVariable("JCODEMUNCH_LAUNCH_ID",$runtimeLaunchId,"Process")
        [Environment]::SetEnvironmentVariable("PYTHONNOUSERSITE","1","Process")
        [Environment]::SetEnvironmentVariable("PYTHONDONTWRITEBYTECODE","1","Process")
        if($test){
            $fixture=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\tests\hybrid_runtime\fixtures\fake_jcodemunch.py"))
            $python=[Environment]::GetEnvironmentVariable("JCODEMUNCH_TEST_PYTHON")
            if([string]::IsNullOrWhiteSpace($python)){$python=(Get-Command python -ErrorAction SilentlyContinue).Source}
            if([string]::IsNullOrWhiteSpace($python)-or -not(Test-Path -LiteralPath $fixture)){Throw-LifecycleError "runtime_unavailable" "The disposable jCodeMunch runtime fixture is unavailable."}
            if([Environment]::GetEnvironmentVariable("JCODEMUNCH_TEST_CHILD_PROCESS") -eq "1"){
                $launcher=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\tests\hybrid_runtime\fixtures\fake_jcodemunch_launcher.ps1"))
                $proc=Start-Process -FilePath pwsh -ArgumentList @("-NoProfile","-NonInteractive","-File",$launcher,"-Python",$python,"-Fixture",$fixture,"-Port",[string]$Port,"-HostName","127.0.0.1","-ReadyFile",$ready,"-IdentityFile",$idfile,"-StopFile",$stop) -PassThru -WindowStyle Hidden -ErrorAction Stop
            }else{
                $proc=Start-Process -FilePath $python -ArgumentList @($fixture,"--port",[string]$Port,"--host","127.0.0.1","--ready-file",$ready,"--identity-file",$idfile,"--stop-file",$stop) -PassThru -WindowStyle Hidden -ErrorAction Stop
            }
        }else{
            $manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot "versions.json") -Raw -Encoding utf8|ConvertFrom-Json
            $runtimeRoot=Join-Path $StateRoot "runtimes"
            $verifyArguments=@{RuntimeRoot=$runtimeRoot}
            if($Command-ne"configure"){
                $verifyArguments.IntegrationRoot=$PSScriptRoot
                $verifyArguments.ReceiptPath=Join-Path $StateRoot "install-receipt.json"
            }
            try{$verified=& (Join-Path $PSScriptRoot "verify-runtime.ps1") @verifyArguments}
            catch{Throw-LifecycleError "runtime_integrity" "The pinned companion composition failed prelaunch verification." @{reason=$_.Exception.Message}}
            if($LASTEXITCODE-ne0){Throw-LifecycleError "runtime_integrity" "The pinned companion composition failed prelaunch verification."}
            $versionRoot=Join-Path (Join-Path $runtimeRoot ([string]$manifest.jcodemunch.package)) ([string]$manifest.jcodemunch.version)
            $exe=Join-Path $versionRoot (([string]$manifest.jcodemunch.executable) -replace '/', [IO.Path]::DirectorySeparatorChar)
            if(-not(Test-Path -LiteralPath $exe -PathType Leaf)){Throw-LifecycleError "runtime_unavailable" "The pinned jCodeMunch runtime is missing at $exe; run stage-runtime.ps1 before client cutover." @{path=$exe}}
            $runtimeArgs=[Collections.Generic.List[string]]::new()
            foreach($argument in @($manifest.jcodemunch.serve_args)){[void]$runtimeArgs.Add([string]$argument)}
            [void]$runtimeArgs.Add("--port");[void]$runtimeArgs.Add([string]$Port)
            $proc=Start-Process -FilePath $exe -ArgumentList @($runtimeArgs) -PassThru -WindowStyle Hidden -ErrorAction Stop
        }
    }
    finally {
        foreach($name in $runtimeEnvironment){[Environment]::SetEnvironmentVariable($name,$previousRuntimeEnvironment[$name],"Process")}
    }
    $launcherSnapshot=$null
    $snapshotClock=[Diagnostics.Stopwatch]::StartNew()
    while($snapshotClock.ElapsedMilliseconds -lt 2000 -and $null -eq $launcherSnapshot){$launcherSnapshot=Get-ProcessSnapshot $proc.Id;if($null -eq $launcherSnapshot){Start-Sleep -Milliseconds 50}}
    if($null -eq $launcherSnapshot){Throw-LifecycleError "bootstrap_failed" "Unable to capture the launched process identity."}
    $launchedDescendants=@{}
    $i=$null
    $runtimePid=0
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while($clock.ElapsedMilliseconds -lt $RuntimeStartupTimeoutMilliseconds){
        Start-Sleep -Milliseconds 100
        foreach($entry in @(Get-LaunchedProcessDescendants $launcherSnapshot)){$launchedDescendants["$([int]$entry.snapshot.pid)|$([string]$entry.snapshot.process_start_time)"]=$entry}
        $i=Probe-Runtime $Port
        if($null -ne $i){
            $runtimePid=[int]$i.pid
            if($runtimePid -eq $proc.Id -or (Test-ProcessDescendant -ChildProcessId $runtimePid -AncestorProcessId $proc.Id)){break}
        }
        if($proc.HasExited){break}
    }
    $runtimeOwned=$null -ne $i -and [string]$i.launch_id -eq $runtimeLaunchId -and ($runtimePid -eq $proc.Id -or (Test-ProcessDescendant -ChildProcessId $runtimePid -AncestorProcessId $proc.Id))
    if(-not$runtimeOwned){
        $cleanup=Stop-LaunchedProcessTree -LauncherSnapshot $launcherSnapshot -KnownDescendants @($launchedDescendants.Values) -StopPath $stop
        if(-not $cleanup.stopped){Throw-LifecycleError "bootstrap_cleanup_failed" "The launched jCodeMunch process tree failed identity-safe cleanup." @{process_action="incomplete"}}
        Throw-LifecycleError "bootstrap_failed" "jCodeMunch did not pass health and runtime identity probes."
    }
    $snapshot=Get-ProcessSnapshot $runtimePid
    if($null -eq $snapshot){
        $cleanup=Stop-LaunchedProcessTree -LauncherSnapshot $launcherSnapshot -KnownDescendants @($launchedDescendants.Values) -StopPath $stop
        if(-not $cleanup.stopped){Throw-LifecycleError "bootstrap_cleanup_failed" "The launched jCodeMunch process tree failed identity-safe cleanup." @{process_action="incomplete"}}
        Throw-LifecycleError "bootstrap_failed" "Unable to capture the complete owned process tuple."
    }
    if([string]::IsNullOrWhiteSpace([string]$snapshot.executable_path) -or -not(Test-Path -LiteralPath $snapshot.executable_path -PathType Leaf)){
        $cleanup=Stop-LaunchedProcessTree -LauncherSnapshot $launcherSnapshot -KnownDescendants @($launchedDescendants.Values) -StopPath $stop
        if(-not $cleanup.stopped){Throw-LifecycleError "bootstrap_cleanup_failed" "The launched jCodeMunch process tree failed identity-safe cleanup." @{process_action="incomplete"}}
        Throw-LifecycleError "bootstrap_failed" "Unable to hash the owned runtime executable."
    }
    $executableSha256=(Get-FileHash -LiteralPath $snapshot.executable_path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
    $userSid=[Environment]::UserName
    try {$userSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value} catch {}
    return [pscustomobject]@{
        schema_version=1
        owner_id=[Guid]::NewGuid().ToString("N")
        user_sid=$userSid
        pid=$runtimePid
        process_start_time=[string]$snapshot.process_start_time
        executable_path=$snapshot.executable_path
        executable_sha256=$executableSha256
        command_line=$snapshot.command_line
        launcher_pid=$proc.Id
        launcher_process_start_time=[string]$launcherSnapshot.process_start_time
        launcher_executable_path=$launcherSnapshot.executable_path
        launcher_command_line=$launcherSnapshot.command_line
        command_fingerprint=[string]$i.command_fingerprint
        runtime_version=[string]$i.runtime_version
        port=$Port
        runtime_identity=[string]$i.runtime_identity
        launch_id=[string]$i.launch_id
        server_name="jcodemunch"
        transport="streamable-http"
        bind_host="127.0.0.1"
        ready_path=$ready
        identity_path=$idfile
        stop_path=$stop
        acquired_at=[DateTime]::UtcNow.ToString("o")
    }
}function Ensure-Port {
    $state=Read-State "port";if($null -ne $state){return [int]$state.port};$saved=@($CandidatePorts,$CandidateRanges,$DenylistedPorts,$ReservedRanges,$DynamicRanges,$ExcludedRanges);$script:CandidatePorts=@();$script:CandidateRanges=@();$script:DenylistedPorts=@();$script:ReservedRanges=@();$script:DynamicRanges=@("49152-65535");$script:ExcludedRanges=@();$out=Configure-Lifecycle;$script:CandidatePorts=$saved[0];$script:CandidateRanges=$saved[1];$script:DenylistedPorts=$saved[2];$script:ReservedRanges=$saved[3];$script:DynamicRanges=$saved[4];$script:ExcludedRanges=$saved[5];return [int]$out.endpoint.port
}function Get-Leases {$state=Read-State "leases";if($null -eq $state){return @()};return @($state.leases)}
function Save-Leases {param([object[]]$Leases);Write-State "leases" ([pscustomobject]@{schema_version=1;leases=@($Leases)})}
function Start-ShutdownWatcher {
    param([Parameter(Mandatory=$true)][string]$Generation)
    $watcherScript=Join-Path $PSScriptRoot "watch-shutdown.ps1"
    if(-not(Test-Path -LiteralPath $watcherScript -PathType Leaf)){Throw-LifecycleError "watcher_unavailable" "The shutdown watcher script is unavailable."}
    $pwsh=(Get-Command pwsh -ErrorAction SilentlyContinue).Source
    if([string]::IsNullOrWhiteSpace($pwsh)){Throw-LifecycleError "watcher_unavailable" "PowerShell 7 is required for the shutdown watcher."}
    $quote={param([string]$Value);return "'$($Value.Replace("'","''"))'"}
    $command="& $(& $quote $watcherScript) -LifecycleScript $(& $quote $PSCommandPath) -StateRoot $(& $quote $StateRoot) -ConfigRoot $(& $quote $ConfigRoot) -Generation $(& $quote $Generation)"
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $doubleQuote=[char]34
    $commandLine="$doubleQuote$pwsh$doubleQuote -NoProfile -NonInteractive -EncodedCommand $encoded"
    $startup=New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ShowWindow=[uint16]0}
    $created=Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=$commandLine;CurrentDirectory=$PSScriptRoot;ProcessStartupInformation=$startup} -ErrorAction Stop
    if($null -eq $created -or [int]$created.ReturnValue -ne 0 -or [int]$created.ProcessId -lt 1){Throw-LifecycleError "watcher_start_failed" "The shutdown watcher could not be started."}
    $watcherPid=[int]$created.ProcessId
    $watcherStart=$null
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while($clock.ElapsedMilliseconds-lt2000-and$null-eq$watcherStart){$watcherStart=Get-ProcessStart $watcherPid;if($null-eq$watcherStart){Start-Sleep -Milliseconds 50}}
    if($null-eq$watcherStart){Throw-LifecycleError "watcher_start_failed" "The shutdown watcher identity could not be captured."}
    return [pscustomobject]@{pid=$watcherPid;process_start_time=$watcherStart}
}
function Wait-ShutdownWatcherExit {
    param([AllowNull()][psobject]$Shutdown)
    if($null -eq $Shutdown -or $null -eq $Shutdown.PSObject.Properties["watcher"] -or $null -eq $Shutdown.watcher){return $true}
    $watcherPid=0
    if(-not[int]::TryParse([string]$Shutdown.watcher.pid,[ref]$watcherPid)-or$watcherPid-lt1){return $true}
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while($clock.ElapsedMilliseconds-lt2000-and$null-ne(Get-Process -Id $watcherPid -ErrorAction SilentlyContinue)){Start-Sleep -Milliseconds 50}
    return $null -eq (Get-Process -Id $watcherPid -ErrorAction SilentlyContinue)
}
function Start-LeaseMonitor {
    param([Parameter(Mandatory=$true)][psobject]$Owner)
    if([Environment]::GetEnvironmentVariable("JCODEMUNCH_LIFECYCLE_TEST_MODE")-eq"1"-and[Environment]::GetEnvironmentVariable("JCODEMUNCH_TEST_MONITOR_FAILURE")-eq"1"){
        Throw-LifecycleError "monitor_start_failed" "Injected stale-lease monitor failure for orphan-cleanup verification."
    }
    $existing=Read-State "monitor"
    if($null -ne $existing -and [string]$existing.owner_id -eq [string]$Owner.owner_id -and (Test-ProcessSnapshotMatch $existing)){return $existing}
    $monitorScript=Join-Path $PSScriptRoot "watch-leases.ps1"
    if(-not(Test-Path -LiteralPath $monitorScript -PathType Leaf)){Throw-LifecycleError "monitor_unavailable" "The stale-lease monitor script is unavailable."}
    $pwsh=(Get-Command pwsh -ErrorAction SilentlyContinue).Source
    if([string]::IsNullOrWhiteSpace($pwsh)){Throw-LifecycleError "monitor_unavailable" "PowerShell 7 is required for the stale-lease monitor."}
    $generation=[Guid]::NewGuid().ToString("N")
    $quote={param([string]$Value);return "'$($Value.Replace("'","''"))'"}
    $command="& $(& $quote $monitorScript) -LifecycleScript $(& $quote $PSCommandPath) -StateRoot $(& $quote $StateRoot) -ConfigRoot $(& $quote $ConfigRoot) -OwnerId $(& $quote ([string]$Owner.owner_id)) -Generation $(& $quote $generation) -GraceSeconds $GraceSeconds"
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $doubleQuote=[char]34;$commandLine="$doubleQuote$pwsh$doubleQuote -NoProfile -NonInteractive -EncodedCommand $encoded"
    $startup=New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ShowWindow=[uint16]0}
    $created=Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=$commandLine;CurrentDirectory=$PSScriptRoot;ProcessStartupInformation=$startup} -ErrorAction Stop
    if($null -eq $created -or [int]$created.ReturnValue -ne 0 -or [int]$created.ProcessId -lt 1){Throw-LifecycleError "monitor_start_failed" "The stale-lease monitor could not be started."}
    $snapshot=$null;$clock=[Diagnostics.Stopwatch]::StartNew()
    while($clock.ElapsedMilliseconds-lt2000-and$null-eq$snapshot){$snapshot=Get-ProcessSnapshot ([int]$created.ProcessId);if($null-eq$snapshot){Start-Sleep -Milliseconds 50}}
    if($null -eq $snapshot){Throw-LifecycleError "monitor_start_failed" "The stale-lease monitor identity could not be captured."}
    $monitor=[pscustomobject]@{schema_version=1;generation=$generation;owner_id=[string]$Owner.owner_id;pid=[int]$snapshot.pid;process_start_time=[string]$snapshot.process_start_time;executable_path=$snapshot.executable_path;command_line=$snapshot.command_line;grace_seconds=$GraceSeconds;started_at=[DateTime]::UtcNow.ToString("o")}
    Write-State "monitor" $monitor
    return $monitor
}
function Acquire-Lifecycle {
    if($Client -notin @("codex","claude","pi")){Throw-LifecycleError "invalid_client" "Unsupported client '$Client'."}
    if([string]::IsNullOrWhiteSpace($SessionId)){Throw-LifecycleError "invalid_session" "SessionId is required."}
    $workspace=Get-WorkspaceIdentity $WorkspaceRoot
    $leasePid=if($ClientPid -gt 0){$ClientPid}else{$PID}
    $leaseProcessStart=Get-ProcessStart $leasePid
    if($null -eq $leaseProcessStart){Throw-LifecycleError "invalid_client_pid" "ClientPid '$leasePid' does not identify a live process with a verifiable start time." @{client=$Client;session_id=$SessionId;client_pid=$leasePid}}
    $port=Ensure-Port
    $owner=Read-State "owner"
    $reused=$false
    if($null -ne $owner){
        [void](Assert-Owned $owner "reuse or repair the recorded jCodeMunch runtime")
        $reused=$true
    }
    $startedHere=$false
    if($null -eq $owner){
        $runtime=Probe-Runtime $port
        $availability=Test-PortAvailable $port
        if($null -ne $runtime -or -not $availability.available){
            $details=@{port=$port;action="reselect-port";process_action="none";listener_check="foreign-or-unverified"}
            if($null -ne $availability.PSObject.Properties["address_family"]){$details.address_family=$availability.address_family}
            if($null -ne $availability.PSObject.Properties["occupants"]){$details.occupants=$availability.occupants}
            if($null -ne $runtime){$details.runtime_identity=$runtime}
            Throw-LifecycleError "port_collision" "Configured port is occupied by a foreign or unverified listener; no process action was taken. Run explicit port reselection and restart clients." $details
        }
        $owner=Start-Runtime $port
        $startedHere=$true
    }
    try {
        if($startedHere){
            Write-State "owner" $owner
            if(-not(Test-Path -LiteralPath (Get-JCodeMunchStatePath -Name owner -StateRoot $StateRoot))){Write-JCodeMunchAtomicText -Path (Get-JCodeMunchStatePath -Name owner -StateRoot $StateRoot) -Text ($owner|ConvertTo-Json -Depth 20)|Out-Null}
        }
        $leases=@(Get-Leases)
    $existing=@($leases|Where-Object{
        [string]$_.client -eq $Client -and
        [string]$_.session_id -eq $SessionId -and
        ($null -eq $_.PSObject.Properties["workspace_id"] -or [string]$_.workspace_id -eq [string]$workspace.workspace_id)
    })
    $idempotent=$existing.Count -gt 0
    $now=[DateTime]::UtcNow.ToString("o")
    if($idempotent){
        foreach($lease in $existing){
            $lease|Add-Member -NotePropertyName workspace_root -NotePropertyValue $workspace.workspace_root -Force
            $lease|Add-Member -NotePropertyName workspace_id -NotePropertyValue $workspace.workspace_id -Force
            $lease|Add-Member -NotePropertyName pid -NotePropertyValue $leasePid -Force
            $lease|Add-Member -NotePropertyName process_start_time -NotePropertyValue $leaseProcessStart -Force
            $lease.last_seen=$now
        }
    }else{
        $leases+=[pscustomobject]@{
            client=$Client
            session_id=$SessionId
            workspace_root=$workspace.workspace_root
            workspace_id=$workspace.workspace_id
            pid=$leasePid
            process_start_time=$leaseProcessStart
            acquired_at=$now
            last_seen=$now
        }
    }
    Save-Leases $leases
    $shutdown=Read-State "shutdown"
    $cancelled=$null -ne $shutdown
    $watcherExited=$null
    if($cancelled){
        Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name shutdown -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue
        $watcherExited=Wait-ShutdownWatcherExit $shutdown
    }
        $monitor=Start-LeaseMonitor $owner
        return [pscustomobject]@{server=$owner;endpoint="http://127.0.0.1:$port";workspace=$workspace;idempotent=$idempotent;reused=$reused;shutdown_cancelled=$cancelled;cancelled_watcher_exited=$watcherExited;monitor=$monitor;leases=@(Get-Leases)}
    } catch {
        $cause=$_.Exception
        if($startedHere-and$null-ne$owner){
            try{
                [void](Stop-ValidatedRuntime $owner)
                foreach($name in @("owner","monitor","shutdown")){Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name $name -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue}
                Save-Leases @()
            }catch{
                Throw-LifecycleError "bootstrap_cleanup_failed" "Acquisition failed and the newly started exact runtime could not be cleaned up safely." @{original_error=$cause.Message;cleanup_error=$_.Exception.Message;process_action="fail-closed"}
            }
        }
        throw $cause
    }
}
function Release-Lifecycle {
    $workspace=Get-WorkspaceIdentity $WorkspaceRoot
    $leases=@(Get-Leases)|Where-Object{
        -not(
            [string]$_.client -eq $Client -and
            [string]$_.session_id -eq $SessionId -and
            ($null -eq $_.PSObject.Properties["workspace_id"] -or [string]$_.workspace_id -eq [string]$workspace.workspace_id)
        )
    }
    Save-Leases @($leases)
    $watcherGeneration=$null
    if(@($leases).Count -eq 0){
        $generation=[Guid]::NewGuid().ToString("N")
        $shutdown=[pscustomobject]@{schema_version=1;generation=$generation;armed=$false;shutdown_at=[DateTime]::UtcNow.AddSeconds($GraceSeconds).ToString("o");grace_seconds=$GraceSeconds}
        Write-State "shutdown" $shutdown
        $watcherGeneration=$generation
    }
    return [pscustomobject]@{server_action="keep";remaining_leases=@($leases).Count;workspace=$workspace;watcher_generation=$watcherGeneration;grace_seconds=$GraceSeconds}
}
function Stop-ValidatedRuntime {
    param([Parameter(Mandatory=$true)][psobject]$Owner)
    [void](Assert-Owned $Owner "stop the recorded jCodeMunch runtime")
    New-Item -ItemType File -Path $Owner.stop_path -Force|Out-Null
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while($clock.ElapsedMilliseconds -lt 3000 -and $null -ne (Get-ProcessStart ([int]$Owner.pid))){Start-Sleep -Milliseconds 100}
    if($null -ne (Get-ProcessStart ([int]$Owner.pid))){
        $validation=Get-OwnershipValidation $Owner
        if(-not $validation.valid){Throw-LifecycleError "ownership_mismatch" "Runtime ownership changed before the stop action; no process action was taken. Inspect the failed checks and explicitly reselect the port if required." @{process_action="none";action="inspect-and-reselect";failed_checks=@($validation.failed_checks)}}
        Stop-Process -Id ([int]$Owner.pid) -Force -ErrorAction Stop
    }
    return [pscustomobject]@{server_action="stop";process_action="stopped";pid=[int]$Owner.pid}
}
function Stop-Lifecycle {
    if(-not [string]::IsNullOrWhiteSpace($OwnershipFixture)){Throw-LifecycleError "ownership_mismatch" "Ownership proof field '$OwnershipFixture' does not match." @{field=$OwnershipFixture;process_action="none"}}
    $owner=Read-State "owner"
    if($null -eq $owner){return [pscustomobject]@{server_action="none";process_action="none"}}
    $result=Stop-ValidatedRuntime $owner
    Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name owner -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name monitor -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue
    return $result
}
function Clean-Lifecycle {$shutdown=Read-State "shutdown";if(-not[string]::IsNullOrWhiteSpace($ExpectedShutdownGeneration)-and($null-eq$shutdown-or[string]$shutdown.generation-ne$ExpectedShutdownGeneration)){return [pscustomobject]@{server_action="cancelled";process_action="none";reason="shutdown_generation_changed"}};$leases=@(Repair-StaleLeases);if($leases.Count -gt 0){return [pscustomobject]@{server_action="keep";remaining_leases=$leases.Count}};if($null -eq $shutdown -and -not $Now){return [pscustomobject]@{server_action="keep";remaining_leases=0}};if($null -ne $shutdown -and -not $Now -and (ConvertTo-JCodeMunchUtc $shutdown.shutdown_at) -gt [DateTime]::UtcNow){return [pscustomobject]@{server_action="keep";remaining_leases=0}};$owner=Read-State "owner";if($null -ne $owner){[void](Assert-Owned $owner "clean the recorded jCodeMunch runtime")};$r=Stop-Lifecycle;Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name shutdown -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue;return $r }
function Status-Lifecycle {$port=Read-State "port";$owner=Read-State "owner";$leases=@(Get-Leases);$shutdown=Read-State "shutdown";$monitor=Read-State "monitor";$proof=if($null -ne $owner){Get-OwnershipValidation $owner}else{[pscustomobject]@{valid=$false;failed_checks=@("owner_record");message="No recorded owner tuple exists."}};$healthy=[bool]$proof.valid;$ep=if($null -ne $port){"http://127.0.0.1:$([int]$port.port)"}else{$null};return [pscustomobject]@{schema_version=1;configured=$null -ne $port;endpoint=$ep;server=$owner;healthy=$healthy;leases=$leases;shutdown=$shutdown;monitor=$monitor;diagnostics=[pscustomobject]@{ownership_proof=[pscustomobject]@{recorded=$null -ne $owner;validated=$healthy;failed_checks=@($proof.failed_checks);message=$proof.message;checks=@("pid","process_start_time","executable_path","executable_sha256","runtime_version","command_line","command_fingerprint","port","runtime_identity","launch_id")};token_store=if(Test-Path -LiteralPath (Get-JCodeMunchSecretPath -StateRoot $StateRoot) -PathType Leaf){"configured"}else{"missing"};log_path=(Get-JCodeMunchLogPath -StateRoot $StateRoot)}} }
function Rollback-Lifecycle {
    $backups=Read-State "backups"
    if($null -eq $backups){return [pscustomobject]@{restored=0;deleted=0;failed=0;failures=@()}}
    $leases=@(Repair-StaleLeases)
    if($leases.Count -gt 0){Throw-LifecycleError "rollback_requires_idle" "Rollback is refused while one or more live or unverifiable client leases remain." @{action="release-clients-before-rollback";remaining_leases=$leases.Count}}
    $result=Restore-ConfigBackups $backups
    if($result.failed -gt 0){Throw-LifecycleError "rollback_incomplete" "One or more configuration targets could not be restored." @{rollback=$result}}
    $backups|Add-Member -NotePropertyName status -NotePropertyValue "rolled_back" -Force
    Write-State "backups" $backups
    $shutdown=Read-State "shutdown"
    $monitor=Read-State "monitor"
    Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name shutdown -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue
    $owner=Read-State "owner"
    if($null -ne $owner){[void](Stop-Lifecycle)}else{Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name monitor -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue}
    Save-Leases @()
    if($null -ne $shutdown){[void](Wait-ShutdownWatcherExit $shutdown)}
    if($null -ne $monitor){[void](Wait-ShutdownWatcherExit ([pscustomobject]@{watcher=$monitor}))}
    return $result
}
$result=$null;$exitCode=0
# USERPROFILE is intentionally referenced only as a legacy environment marker;
# test mode always requires explicit disposable StateRoot and ConfigRoot values.
try {
    Initialize-JCodeMunchStateRoot -StateRoot $StateRoot | Out-Null
    $result = Invoke-JCodeMunchStateLock -StateRoot $StateRoot -ScriptBlock {
        Write-Diagnostic -Event "command_started" -Details @{command=$Command;client=$Client;session_id=$SessionId}
        try {
            $commandResult = switch ($Command) {
                "configure" { Configure-Lifecycle }
                "acquire" { Acquire-Lifecycle }
                "release" { Release-Lifecycle }
                "heartbeat" { Acquire-Lifecycle }
                "status" { Status-Lifecycle }
                "stop" { Stop-Lifecycle }
                "clean" { Clean-Lifecycle }
                "rollback" { Rollback-Lifecycle }
            }
            Write-Diagnostic -Event "command_succeeded" -Details @{command=$Command;result=$commandResult}
            return $commandResult
        } catch {
            Write-Diagnostic -Event "command_failed" -Level "error" -Details @{command=$Command;message=$_.Exception.Message}
            throw
        }
    }
    if($Command -eq "release" -and $null -ne $result.PSObject.Properties["watcher_generation"] -and -not [string]::IsNullOrWhiteSpace([string]$result.watcher_generation)){
        $generation=[string]$result.watcher_generation
        $watcher=$null
        try {
            $watcher=Start-ShutdownWatcher $generation
            $armed=Invoke-JCodeMunchStateLock -StateRoot $StateRoot -ScriptBlock {
                $shutdown=Read-State "shutdown"
                if($null -eq $shutdown -or [string]$shutdown.generation -ne $generation){return $false}
                $shutdown|Add-Member -NotePropertyName watcher -NotePropertyValue $watcher -Force
                $shutdown.armed=$true
                Write-State "shutdown" $shutdown
                return $true
            }
            if(-not $armed){[void](Wait-ShutdownWatcherExit ([pscustomobject]@{watcher=$watcher}))}
            $result|Add-Member -NotePropertyName watcher -NotePropertyValue $watcher -Force
            $result|Add-Member -NotePropertyName watcher_armed -NotePropertyValue ([bool]$armed) -Force
            $result.PSObject.Properties.Remove("watcher_generation")
        } catch {
            [void](Invoke-JCodeMunchStateLock -StateRoot $StateRoot -ScriptBlock {
                $shutdown=Read-State "shutdown"
                if($null -ne $shutdown -and [string]$shutdown.generation -eq $generation){
                    Remove-Item -LiteralPath (Get-JCodeMunchStatePath -Name shutdown -StateRoot $StateRoot) -Force -ErrorAction SilentlyContinue
                }
            })
            throw
        }
    }
}
catch {
    $exitCode=1;$e=$_.Exception;$source=$e;$code=$null
    while($null -eq $code -and $null -ne $source){if($null -ne $source.Data["code"]){$code=[string]$source.Data["code"]};if($null -eq $code){$source=$source.InnerException}}
    $d=[ordered]@{code=if($null -ne $code){$code}else{"lifecycle_error"};message=(Protect-JCodeMunchDiagnosticText -Text $e.Message)}
    $source=$e
    while($null -ne $source){foreach($k in @("port","action","field","process_action","rejections","range_policy","rejection","failed_checks","rollback")){if($null -ne $source.Data[$k]){$d[$k]=$source.Data[$k]}};$source=$source.InnerException}
    $pa=if($d.Contains("process_action")){$d["process_action"]}else{"none"};$result=[ordered]@{error=$d;process_action=$pa};try{Write-Diagnostic -Event "command_failed" -Level "error" -Details $result}catch{}
}
$result|ConvertTo-Json -Depth 30 -Compress
exit $exitCode
