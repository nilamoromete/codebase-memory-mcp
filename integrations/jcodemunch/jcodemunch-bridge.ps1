#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("codex", "claude", "pi")]
    [string] $Client,
    [string] $SessionId = ([Guid]::NewGuid().ToString("N")),
    [string] $WorkspaceRoot = (Get-Location).Path,
    [string] $StateRoot,
    [string] $ConfigRoot,
    [int] $ParentPid = 0,
    [ValidateRange(0, 600)]
    [int] $GraceSeconds = 15
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($StateRoot)) {
    $StateRoot = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "CodebaseMemoryMcp\companions\jcodemunch"
}
if ([string]::IsNullOrWhiteSpace($ConfigRoot)) { $ConfigRoot = $StateRoot }

$lifecycleScript = Join-Path $PSScriptRoot "jcodemunch-lifecycle.ps1"
$modulePath = Join-Path $PSScriptRoot "JCodeMunchLifecycle.psm1"
$manifestPath = Join-Path $PSScriptRoot "versions.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding utf8 | ConvertFrom-Json
$pwsh = (Get-Command pwsh -ErrorAction Stop).Source

function Get-ProcessStartIdentity {
    param([Parameter(Mandatory = $true)][int] $ProcessId)
    try {
        $process = Get-Process -Id $ProcessId -ErrorAction Stop
        return $process.StartTime.ToUniversalTime().Ticks.ToString()
    }
    catch { return $null }
}

function Get-BridgeParentPid {
    if ($ParentPid -gt 0) { return $ParentPid }
    try {
        return [int](Get-CimInstance Win32_Process -Filter "ProcessId=$PID" -ErrorAction Stop).ParentProcessId
    }
    catch { return 0 }
}

function Invoke-LifecycleCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Command
    )

    $arguments = [Collections.Generic.List[string]]::new()
    foreach ($value in @(
        "-NoProfile", "-NonInteractive", "-File", $lifecycleScript,
        "-Command", $Command, "-StateRoot", $StateRoot,
        "-ConfigRoot", $ConfigRoot, "-Client", $Client,
        "-SessionId", $SessionId, "-WorkspaceRoot", $WorkspaceRoot,
        "-GraceSeconds", [string]$GraceSeconds, "-Json"
    )) { $arguments.Add([string]$value) }
    if ($Command -in @("acquire", "heartbeat")) {
        $arguments.Add("-ClientPid")
        $arguments.Add([string]$PID)
    }

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $pwsh
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $arguments) { [void]$startInfo.ArgumentList.Add($argument) }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "Unable to start the lifecycle command." }
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        $safeError = Protect-JCodeMunchDiagnosticText -Text $stderr
        $safeOutput = Protect-JCodeMunchDiagnosticText -Text $stdout
        throw "jCodeMunch lifecycle $Command failed: $safeError$safeOutput"
    }
    try { return $stdout | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "jCodeMunch lifecycle $Command returned invalid JSON." }
}

function Assert-InstalledRuntime {
    if ([Environment]::GetEnvironmentVariable("JCODEMUNCH_LIFECYCLE_TEST_MODE") -eq "1") { return }

    $verifyScript = Join-Path $PSScriptRoot "verify-runtime.ps1"
    $runtimeRoot = Join-Path $StateRoot "runtimes"
    $receiptPath = Join-Path $StateRoot "install-receipt.json"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $pwsh
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in @(
        "-NoProfile", "-NonInteractive", "-File", $verifyScript,
        "-RuntimeRoot", $runtimeRoot, "-ReceiptPath", $receiptPath,
        "-IntegrationRoot", $PSScriptRoot
    )) { [void]$startInfo.ArgumentList.Add([string]$argument) }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "Unable to start installed-runtime verification." }
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        $safeError = Protect-JCodeMunchDiagnosticText -Text $stderr
        $safeOutput = Protect-JCodeMunchDiagnosticText -Text $stdout
        throw "Installed jCodeMunch composition failed verification: $safeError$safeOutput"
    }
    try { $verification = $stdout | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "Installed-runtime verification returned invalid JSON." }
    if (-not [bool]$verification.verified) { throw "Installed-runtime verification was not affirmative." }
}

Import-Module $modulePath -Force
$acquired = $null
$proxyExitCode = 1
$proxyProcess = $null
$bridgeInput = $null
$bridgeParentPid = Get-BridgeParentPid
$bridgeParentStart = if ($bridgeParentPid -gt 0) { Get-ProcessStartIdentity -ProcessId $bridgeParentPid } else { $null }

try {
    # Reused runtime processes still need fresh on-disk verification. Otherwise
    # a modified proxy could be launched indefinitely after the first acquire.
    Assert-InstalledRuntime
    $acquired = Invoke-LifecycleCommand -Command "acquire"
    $token = Get-JCodeMunchSecret -StateRoot $StateRoot
    if ([string]::IsNullOrWhiteSpace($token)) { throw "The lifecycle token store is empty." }

    $proxyRoot = Join-Path (Join-Path (Join-Path $StateRoot "runtimes") ([string]$manifest.stdio_proxy.package)) ([string]$manifest.stdio_proxy.version)
    $proxyEntrypoint = Join-Path $proxyRoot (([string]$manifest.stdio_proxy.entrypoint) -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $proxyEntrypoint -PathType Leaf)) {
        throw "The pinned mcp-remote entrypoint is missing; run stage-runtime.ps1 before client cutover."
    }
    $node = (Get-Command node -ErrorAction Stop).Source
    $endpoint = ([string]$acquired.endpoint).TrimEnd('/') + "/mcp"

    $proxyStart = [Diagnostics.ProcessStartInfo]::new()
    $workspaceProxy = Join-Path $PSScriptRoot "workspace-proxy.mjs"
    if (-not (Test-Path -LiteralPath $workspaceProxy -PathType Leaf)) {
        throw "The workspace-bound proxy is missing; reinstall the hybrid integration."
    }
    $resolvedWorkspace = [IO.Path]::GetFullPath($WorkspaceRoot)
    if (-not (Test-Path -LiteralPath $resolvedWorkspace -PathType Container)) {
        throw "The client workspace root does not exist: $resolvedWorkspace"
    }

    $proxyStart.FileName = $node
    $proxyStart.UseShellExecute = $false
    $proxyStart.CreateNoWindow = $true
    $proxyStart.RedirectStandardInput = $true
    $proxyStart.RedirectStandardOutput = $true
    $proxyStart.RedirectStandardError = $true
    [void]$proxyStart.Environment.Remove("NODE_OPTIONS")
    [void]$proxyStart.Environment.Remove("NODE_PATH")
    $proxyStart.Environment["JCODEMUNCH_HTTP_TOKEN"] = $token
    foreach ($argument in @(
        $workspaceProxy,
        "--proxy-entrypoint", $proxyEntrypoint,
        "--endpoint", $endpoint,
        "--workspace-root", $resolvedWorkspace
    )) { [void]$proxyStart.ArgumentList.Add([string]$argument) }

    $proxyProcess = [Diagnostics.Process]::new()
    $proxyProcess.StartInfo = $proxyStart
    if (-not $proxyProcess.Start()) { throw "Unable to start the pinned STDIO proxy." }
    $bridgeInput = [Console]::OpenStandardInput()
    $stdinTask = $bridgeInput.CopyToAsync($proxyProcess.StandardInput.BaseStream)
    $stdoutTask = $proxyProcess.StandardOutput.BaseStream.CopyToAsync([Console]::OpenStandardOutput())
    $stderrTask = $proxyProcess.StandardError.BaseStream.CopyToAsync([Console]::OpenStandardError())
    $proxyInputClosed = $false
    while (-not $proxyProcess.WaitForExit(500)) {
        if (-not $proxyInputClosed -and $stdinTask.IsCompleted) {
            try { $stdinTask.GetAwaiter().GetResult() } catch {}
            try { $proxyProcess.StandardInput.Close() } catch {}
            $proxyInputClosed = $true
        }
        if ($bridgeParentPid -gt 0 -and $null -ne $bridgeParentStart) {
            $currentParentStart = Get-ProcessStartIdentity -ProcessId $bridgeParentPid
            if ($currentParentStart -ne $bridgeParentStart) {
                try { $proxyProcess.Kill($true) } catch {}
                $proxyProcess.WaitForExit()
                throw "The client parent process exited; the exact proxy child was stopped."
            }
        }
    }
    if (-not $stdinTask.IsCompleted) {
        try { $bridgeInput.Dispose() } catch {}
        try { [void]$stdinTask.Wait([TimeSpan]::FromSeconds(5)) } catch {}
    }
    try { $proxyProcess.StandardInput.Close() } catch {}
    foreach ($copyTask in @($stdoutTask, $stderrTask)) {
        try { [void]$copyTask.Wait([TimeSpan]::FromSeconds(5)) } catch {}
    }
    $proxyExitCode = $proxyProcess.ExitCode
}
finally {
    if ($null -ne $bridgeInput) {
        try { $bridgeInput.Dispose() } catch {}
    }
    if ($null -ne $proxyProcess -and -not $proxyProcess.HasExited) {
        try { $proxyProcess.Kill($true) } catch {}
        try { $proxyProcess.WaitForExit(5000) } catch {}
    }
    if ($null -ne $acquired) {
        try { [void](Invoke-LifecycleCommand -Command "release") }
        catch {
            $safeMessage = Protect-JCodeMunchDiagnosticText -Text $_.Exception.Message
            [Console]::Error.WriteLine($safeMessage)
            if ($proxyExitCode -eq 0) { $proxyExitCode = 1 }
        }
    }
}

exit $proxyExitCode
