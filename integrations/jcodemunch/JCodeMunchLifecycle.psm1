#Requires -Version 7.2

Set-StrictMode -Version Latest

$script:StateSchemaVersion = 1
$script:DefaultStateDirectoryName = "CodebaseMemoryMcp\companions\jcodemunch"
$script:DefaultStateDocuments = @("port", "owner", "leases", "backups", "shutdown")

function ConvertTo-JCodeMunchFullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    return [IO.Path]::GetFullPath($Path)
}

function Assert-JCodeMunchNoReparseAncestors {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string] $Path)

    $full = [IO.Path]::GetFullPath($Path)
    $cursor = $full
    while (-not [string]::IsNullOrWhiteSpace($cursor)) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Path ancestry contains a link or reparse point: $cursor"
            }
        }
        $parent = [IO.Directory]::GetParent($cursor)
        if ($null -eq $parent -or [string]::Equals($parent.FullName, $cursor, [StringComparison]::OrdinalIgnoreCase)) { break }
        $cursor = $parent.FullName
    }
    return $full
}

function Get-JCodeMunchDefaultStateRoot {
    [CmdletBinding()]
    param()

    $localAppData = [Environment]::GetEnvironmentVariable("LOCALAPPDATA")
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        $localAppData = [Environment]::GetEnvironmentVariable("XDG_STATE_HOME")
    }
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        $localAppData = Join-Path ([Environment]::GetFolderPath("UserProfile")) ".local\state"
    }

    return ConvertTo-JCodeMunchFullPath (Join-Path $localAppData $script:DefaultStateDirectoryName)
}

function Set-JCodeMunchOwnerAcl {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,
        [switch] $File
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Cannot apply private ACL; path does not exist: $Path"
    }

    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        # Use the current user SID rather than a display name. This avoids
        # localization and ambiguous-name ACLs on Windows.
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $sid = $identity.User.Value
        $grant = if ($File) { "*${sid}:(F)" } else { "*${sid}:(OI)(CI)(F)" }
        $target = if ($File) { $Path } else { $Path }
        & icacls.exe $target /inheritance:r /grant:r $grant /c 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to apply user-only ACL to $Path (icacls exit code $LASTEXITCODE)."
        }
        return
    }

    # The implementation is exercised from PowerShell on non-Windows CI too.
    # chmod is the closest user-only equivalent there; failure is fatal.
    & chmod 700 -- $Path 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to apply user-only permissions to $Path."
    }
}

function Initialize-JCodeMunchStateRoot {
    [CmdletBinding()]
    param(
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot)
    )

    $root = ConvertTo-JCodeMunchFullPath $StateRoot
    $secrets = Join-Path $root "secrets"
    $lockPath = Join-Path $root "state.lock"

    # Create and protect the parent directories before opening the lock. This
    # keeps the lock file private without trying to change its ACL while a
    # competing process already has it open.
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        New-Item -ItemType Directory -Path $root -Force | Out-Null
    }
    Set-JCodeMunchOwnerAcl -Path $root

    if (-not (Test-Path -LiteralPath $secrets -PathType Container)) {
        New-Item -ItemType Directory -Path $secrets -Force | Out-Null
    }
    Set-JCodeMunchOwnerAcl -Path $secrets

    if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
        [IO.File]::WriteAllBytes($lockPath, [byte[]]@())
    }
    Set-JCodeMunchOwnerAcl -Path $lockPath -File

    $bootstrapLock = $null
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($null -eq $bootstrapLock -and $deadline.ElapsedMilliseconds -lt 30000) {
        try {
            $bootstrapLock = [IO.FileStream]::new(
                $lockPath,
                [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite,
                [IO.FileShare]::None
            )
        }
        catch [IO.IOException] {
            Start-Sleep -Milliseconds 50
        }
    }
    if ($null -eq $bootstrapLock) {
        throw "Timed out initializing the jCodeMunch state root: $root"
    }
    $bootstrapLock.Dispose()

    [pscustomobject]@{
        Root       = $root
        Secrets    = $secrets
        LockPath   = $lockPath
        StatePaths = [ordered]@{
            Port     = Join-Path $root "port.json"
            Owner    = Join-Path $root "owner.json"
            Leases   = Join-Path $root "leases.json"
            Backups  = Join-Path $root "backups.json"
            Shutdown = Join-Path $root "shutdown.json"
            Monitor  = Join-Path $root "monitor.json"
        }
        SecretPath = Join-Path $secrets "token.json"
    }
}
function Get-JCodeMunchStatePath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("port", "owner", "leases", "backups", "shutdown", "monitor")]
        [string] $Name,
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot)
    )

    $root = ConvertTo-JCodeMunchFullPath $StateRoot
    return Join-Path $root "$Name.json"
}

function Get-JCodeMunchSecretPath {
    [CmdletBinding()]
    param(
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot)
    )

    $root = ConvertTo-JCodeMunchFullPath $StateRoot
    return Join-Path (Join-Path $root "secrets") "token.json"
}

function Get-JCodeMunchLogPath {
    [CmdletBinding()]
    param(
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot)
    )

    $root = ConvertTo-JCodeMunchFullPath $StateRoot
    return Join-Path $root "diagnostics.jsonl"
}

function Protect-JCodeMunchDiagnosticText {
    [CmdletBinding()]
    param(
        [AllowNull()]
        [string] $Text
    )

    if ($null -eq $Text) { return $null }

    $redacted = $Text
    $redacted = [regex]::Replace($redacted, '(?i)(bearer\s+)[A-Za-z0-9._~+/=-]+', '$1<redacted>')
    $redacted = [regex]::Replace(
        $redacted,
        '(?i)\b(token|access[_-]?token|api[_-]?key|client[_-]?secret|password|secret)\s*([=:])\s*("[^"]*"|''[^'']*''|[^\s,;]+)',
        '$1$2<redacted>'
    )
    return $redacted
}

function ConvertTo-JCodeMunchRedactedValue {
    param(
        [AllowNull()]
        [object] $Value,
        [int] $Depth = 0
    )

    if ($null -eq $Value) { return $null }
    if ($Depth -gt 8) { return '<redacted-depth>' }
    if ($Value -is [string]) {
        return Protect-JCodeMunchDiagnosticText -Text ([string]$Value)
    }
    if ($Value -is [ValueType] -or $Value -is [bool]) { return $Value }

    $sensitiveName = '(?i)(token|secret|authorization|password|api[_-]?key|access[_-]?token|client[_-]?secret|content[_-]?base64)'
    if ($Value -is [Collections.IDictionary]) {
        $result = [ordered]@{}
        foreach ($key in $Value.Keys) {
            if ([string]$key -match $sensitiveName) {
                $result[[string]$key] = '<redacted>'
            }
            else {
                $result[[string]$key] = ConvertTo-JCodeMunchRedactedValue -Value $Value[$key] -Depth ($Depth + 1)
            }
        }
        return $result
    }
    if ($Value -is [Collections.IEnumerable]) {
        $items = [Collections.Generic.List[object]]::new()
        foreach ($item in $Value) {
            $items.Add((ConvertTo-JCodeMunchRedactedValue -Value $item -Depth ($Depth + 1)))
        }
        return @($items)
    }

    $objectResult = [ordered]@{}
    foreach ($property in $Value.PSObject.Properties) {
        if ($property.Name -match $sensitiveName) {
            $objectResult[$property.Name] = '<redacted>'
        }
        else {
            $objectResult[$property.Name] = ConvertTo-JCodeMunchRedactedValue -Value $property.Value -Depth ($Depth + 1)
        }
    }
    if ($objectResult.Count -eq 0) { return Protect-JCodeMunchDiagnosticText -Text ([string]$Value) }
    return $objectResult
}

function Write-JCodeMunchDiagnosticLog {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string] $Event,
        [ValidateSet('debug', 'info', 'warning', 'error')]
        [string] $Level = 'info',
        [AllowNull()]
        [object] $Details,
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot)
    )

    # Logging is best-effort, but redaction always happens before writing.
    try {
        $path = Get-JCodeMunchLogPath -StateRoot $StateRoot
        $parent = Split-Path -Parent $path
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        $entry = [ordered]@{
            timestamp = [DateTime]::UtcNow.ToString('o')
            level = $Level
            event = Protect-JCodeMunchDiagnosticText -Text $Event
            details = ConvertTo-JCodeMunchRedactedValue -Value $Details
        }
        $line = ($entry | ConvertTo-Json -Depth 20 -Compress) + [Environment]::NewLine
        [IO.File]::AppendAllText($path, $line, [Text.UTF8Encoding]::new($false))
        Set-JCodeMunchOwnerAcl -Path $path -File
    }
    catch {
        # A log sink must never weaken fail-closed lifecycle behavior.
    }
}

function Write-JCodeMunchAtomicText {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,
        [Parameter(Mandatory = $true)]
        [string] $Text,
        [switch] $Secret,
        [switch] $PreserveAcl
    )

    $destination = ConvertTo-JCodeMunchFullPath $Path
    $parent = Split-Path -Parent $destination
    $existingSddl = $null
    $existingSecurityDescriptor = $null
    if ($PreserveAcl -and [OperatingSystem]::IsWindows() -and
        (Test-Path -LiteralPath $destination -PathType Leaf)) {
        $existingSecurityDescriptor = Get-JCodeMunchSecurityDescriptor -Path $destination
        try { $existingSddl = (Get-Acl -LiteralPath $destination -ErrorAction Stop).Sddl } catch {}
    }
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        if (-not $PreserveAcl) { Set-JCodeMunchOwnerAcl -Path $parent }
    }

    $temp = Join-Path $parent (".{0}.{1}.tmp" -f [IO.Path]::GetFileName($destination), [Guid]::NewGuid().ToString("N"))
    $replacementBackup = Join-Path $parent (".{0}.{1}.metadata.bak" -f [IO.Path]::GetFileName($destination), [Guid]::NewGuid().ToString("N"))
    $encoding = [Text.UTF8Encoding]::new($false)
    try {
        $bytes = $encoding.GetBytes($Text)
        $stream = [IO.FileStream]::new(
            $temp,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None,
            4096,
            [IO.FileOptions]::WriteThrough
        )
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        }
        finally {
            $stream.Dispose()
        }

        $aclPreservedByReplace = $false
        if ((Test-Path -LiteralPath $destination -PathType Leaf) -and $PreserveAcl) {
            try {
                [IO.File]::Replace($temp, $destination, $replacementBackup, $true)
                $aclPreservedByReplace = $true
                Remove-Item -LiteralPath $replacementBackup -Force -ErrorAction SilentlyContinue
            }
            catch [PlatformNotSupportedException] {
                [IO.File]::Move($temp, $destination, $true)
            }
        }
        elseif (Test-Path -LiteralPath $destination -PathType Leaf) {
            # Move with overwrite is an atomic same-volume replacement on
            # modern .NET/Windows and does not require a backup path.
            # The Move-Item fallback keeps the operation usable on older CI.
            try {
                [IO.File]::Move($temp, $destination, $true)
            }
            catch [PlatformNotSupportedException] {
                Move-Item -LiteralPath $temp -Destination $destination -Force
            }
        }
        else {
            [IO.File]::Move($temp, $destination)
        }

        if ($Secret -or -not $PreserveAcl) {
            Set-JCodeMunchOwnerAcl -Path $destination -File
        } elseif ($existingSecurityDescriptor -and [OperatingSystem]::IsWindows()) {
            Set-JCodeMunchSecurityDescriptor -Path $destination -DescriptorBase64 $existingSecurityDescriptor
            if (-not(Test-JCodeMunchSecurityDescriptorEquivalent -ExpectedBase64 $existingSecurityDescriptor -ActualBase64 (Get-JCodeMunchSecurityDescriptor -Path $destination))) {
                throw "Preserved security descriptor verification failed: $destination"
            }
        }
    }
    catch {
        if (Test-Path -LiteralPath $temp -PathType Leaf) {
            Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $replacementBackup -PathType Leaf) {
            Remove-Item -LiteralPath $replacementBackup -Force -ErrorAction SilentlyContinue
        }
        throw
    }
}

function Initialize-JCodeMunchNativeSecurity {
    if($null-ne("JCodeMunch.NativeSecurity"-as[type])){return}
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace JCodeMunch {
    public static class NativeSecurity {
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetFileSecurity(string path, uint information, byte[] descriptor, uint length, out uint needed);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetFileSecurity(string path, uint information, byte[] descriptor);
    }
}
"@
}

function Get-JCodeMunchSecurityDescriptor {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$Path)
    if(-not[OperatingSystem]::IsWindows()){return $null}
    Initialize-JCodeMunchNativeSecurity
    [uint32]$needed=0;[uint32]$information=0x00000007
    [void][JCodeMunch.NativeSecurity]::GetFileSecurity([IO.Path]::GetFullPath($Path),$information,$null,0,[ref]$needed)
    if($needed-lt1){throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())}
    $buffer=[byte[]]::new($needed)
    if(-not[JCodeMunch.NativeSecurity]::GetFileSecurity([IO.Path]::GetFullPath($Path),$information,$buffer,$needed,[ref]$needed)){throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())}
    return [Convert]::ToBase64String($buffer)
}

function Set-JCodeMunchSecurityDescriptor {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][string]$DescriptorBase64)
    if(-not[OperatingSystem]::IsWindows()){return}
    Initialize-JCodeMunchNativeSecurity
    $buffer=[Convert]::FromBase64String($DescriptorBase64);[uint32]$information=0x00000007
    if(-not[JCodeMunch.NativeSecurity]::SetFileSecurity([IO.Path]::GetFullPath($Path),$information,$buffer)){throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())}
}

function Get-JCodeMunchAclBinaryFingerprint {
    param([AllowNull()][Security.AccessControl.GenericAcl] $Acl)
    if ($null -eq $Acl) { return $null }
    $buffer = [byte[]]::new($Acl.BinaryLength)
    $Acl.GetBinaryForm($buffer, 0)
    return [Convert]::ToBase64String($buffer)
}

function Test-JCodeMunchSecurityDescriptorEquivalent {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string] $ExpectedBase64,
        [Parameter(Mandatory = $true)][string] $ActualBase64
    )
    if ($ExpectedBase64 -eq $ActualBase64) { return $true }
    if (-not [OperatingSystem]::IsWindows()) { return $false }
    try {
        $expectedBytes = [Convert]::FromBase64String($ExpectedBase64)
        $actualBytes = [Convert]::FromBase64String($ActualBase64)
        $expected = [Security.AccessControl.RawSecurityDescriptor]::new($expectedBytes, 0)
        $actual = [Security.AccessControl.RawSecurityDescriptor]::new($actualBytes, 0)
        if ([string]$expected.Owner -ne [string]$actual.Owner -or
            [string]$expected.Group -ne [string]$actual.Group) {
            return $false
        }
        $mask = [int][Security.AccessControl.ControlFlags]::DiscretionaryAclPresent -bor
            [int][Security.AccessControl.ControlFlags]::DiscretionaryAclProtected
        if (([int]$expected.ControlFlags -band $mask) -ne
            ([int]$actual.ControlFlags -band $mask)) {
            return $false
        }
        $expectedAcl = Get-JCodeMunchAclBinaryFingerprint $expected.DiscretionaryAcl
        $actualAcl = Get-JCodeMunchAclBinaryFingerprint $actual.DiscretionaryAcl
        return $expectedAcl -eq $actualAcl
    }
    catch { return $false }
}

function Test-JCodeMunchStateDocument {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,
        [Parameter(Mandatory = $true)]
        [psobject] $Document
    )

    $schema = $Document.PSObject.Properties["schema_version"]
    if ($null -eq $schema -or [int]$schema.Value -ne $script:StateSchemaVersion) {
        throw "Unsupported jCodeMunch $Name state schema; expected version $script:StateSchemaVersion."
    }

    switch ($Name) {
        "port" {
            if ($null -eq $Document.PSObject.Properties["port"] -or [int]$Document.port -notin 1..65535) {
                throw "Invalid jCodeMunch port state document."
            }
        }
        "owner" {
            foreach ($required in @("owner_id", "user_sid", "pid", "process_start_time", "executable_sha256", "runtime_version", "port", "runtime_identity", "launch_id", "command_fingerprint")) {
                if ($null -eq $Document.PSObject.Properties[$required] -or [string]::IsNullOrWhiteSpace([string]$Document.$required)) {
                    throw "Invalid jCodeMunch owner state; missing $required."
                }
            }
            $hasExecutable = $null -ne $Document.PSObject.Properties["executable_path"] -and -not [string]::IsNullOrWhiteSpace([string]$Document.executable_path)
            $hasCommand = $null -ne $Document.PSObject.Properties["command_line"] -and -not [string]::IsNullOrWhiteSpace([string]$Document.command_line)
            if (-not $hasExecutable -and -not $hasCommand) {
                throw "Invalid jCodeMunch owner state; executable_path or command_line is required."
            }
            if ([int]$Document.port -notin 1..65535) {
                throw "Invalid jCodeMunch owner state; port is outside 1..65535."
            }
        }
        "leases" {
            if ($null -eq $Document.PSObject.Properties["leases"] -or $Document.leases -isnot [array]) {
                throw "Invalid jCodeMunch leases state document."
            }
        }
        "backups" {
            if ($null -eq $Document.PSObject.Properties["entries"] -or $Document.entries -isnot [array]) {
                throw "Invalid jCodeMunch backups state document."
            }
        }
        "shutdown" {
            if ($null -eq $Document.PSObject.Properties["shutdown_at"]) {
                throw "Invalid jCodeMunch shutdown state document."
            }
        }
    }
}

function Write-JCodeMunchStateDocument {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("port", "owner", "leases", "backups", "shutdown", "monitor")]
        [string] $Name,
        [Parameter(Mandatory = $true)]
        [psobject] $Document,
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot),
        [switch] $LockHeld
    )

    if (-not $LockHeld) {
        return Invoke-JCodeMunchStateLock -StateRoot $StateRoot -ScriptBlock {
            Write-JCodeMunchStateDocument -Name $Name -Document $Document -StateRoot $StateRoot -LockHeld
        }
    }

    Test-JCodeMunchStateDocument -Name $Name -Document $Document
    $path = Get-JCodeMunchStatePath -Name $Name -StateRoot $StateRoot
    $json = $Document | ConvertTo-Json -Depth 20
    Write-JCodeMunchAtomicText -Path $path -Text $json
    return $path
}

function Read-JCodeMunchStateDocument {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("port", "owner", "leases", "backups", "shutdown", "monitor")]
        [string] $Name,
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot)
    )

    $path = Get-JCodeMunchStatePath -Name $Name -StateRoot $StateRoot
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return $null
    }

    try {
        $document = Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
        Test-JCodeMunchStateDocument -Name $Name -Document $document
        return $document
    }
    catch {
        throw "Invalid or corrupt jCodeMunch $Name state document at $path`: $($_.Exception.Message)"
    }
}

function New-JCodeMunchSecret {
    [CmdletBinding()]
    param(
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot),
        [switch] $Force,
        [switch] $LockHeld
    )

    if (-not $LockHeld) {
        return Invoke-JCodeMunchStateLock -StateRoot $StateRoot -ScriptBlock {
            New-JCodeMunchSecret -StateRoot $StateRoot -Force:$Force -LockHeld
        }
    }

    $path = Get-JCodeMunchSecretPath -StateRoot $StateRoot
    if ((Test-Path -LiteralPath $path -PathType Leaf) -and -not $Force) {
        $existing = Get-JCodeMunchSecret -StateRoot $StateRoot
        if ($existing) { return $existing }
    }

    $bytes = [byte[]]::new(32)
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    $token = [Convert]::ToBase64String($bytes).TrimEnd("=").Replace("+", "-").Replace("/", "_")
    $now = [DateTime]::UtcNow.ToString("o")
    $document = [ordered]@{
        schema_version = $script:StateSchemaVersion
        token = $token
        created_at = $now
        rotated_at = $now
    }
    Write-JCodeMunchAtomicText -Path $path -Text ($document | ConvertTo-Json -Depth 5) -Secret
    return $token
}

function Get-JCodeMunchSecret {
    [CmdletBinding()]
    param(
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot)
    )

    $path = Get-JCodeMunchSecretPath -StateRoot $StateRoot
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return $null
    }
    try {
        $document = Get-Content -LiteralPath $path -Raw -Encoding utf8 | ConvertFrom-Json
    }
    catch {
        throw "Invalid or corrupt jCodeMunch secret store at $path."
    }
    if ($null -eq $document.schema_version -or [int]$document.schema_version -ne $script:StateSchemaVersion) {
        throw "Unsupported jCodeMunch secret store schema."
    }
    if ($null -eq $document.token -or [string]::IsNullOrWhiteSpace([string]$document.token)) {
        throw "jCodeMunch secret store is missing its token."
    }
    return [string]$document.token
}

function Enter-JCodeMunchStateLock {
    [CmdletBinding()]
    param(
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot),
        [int] $TimeoutMilliseconds = 30000
    )

    if ($TimeoutMilliseconds -lt 1) { throw "Lock timeout must be positive." }
    $state = Initialize-JCodeMunchStateRoot -StateRoot $StateRoot
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
        try {
            $stream = [IO.FileStream]::new(
                $state.LockPath,
                [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite,
                [IO.FileShare]::None
            )
            return $stream
        }
        catch [IO.IOException] {
            Start-Sleep -Milliseconds 50
        }
    }
    throw "Timed out waiting for the jCodeMunch state lock: $($state.LockPath)"
}

function Exit-JCodeMunchStateLock {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [IO.FileStream] $Lock
    )

    $Lock.Dispose()
}

function Invoke-JCodeMunchStateLock {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock] $ScriptBlock,
        [string] $StateRoot = (Get-JCodeMunchDefaultStateRoot),
        [int] $TimeoutMilliseconds = 30000
    )

    $lock = Enter-JCodeMunchStateLock -StateRoot $StateRoot -TimeoutMilliseconds $TimeoutMilliseconds
    try {
        & $ScriptBlock
    }
    finally {
        Exit-JCodeMunchStateLock -Lock $lock
    }
}

Export-ModuleMember -Function @(
    "Get-JCodeMunchDefaultStateRoot",
    "Assert-JCodeMunchNoReparseAncestors",
    "Set-JCodeMunchOwnerAcl",
    "Initialize-JCodeMunchStateRoot",
    "Get-JCodeMunchStatePath",
    "Get-JCodeMunchSecretPath",
    "Get-JCodeMunchLogPath",
    "Protect-JCodeMunchDiagnosticText",
    "Write-JCodeMunchDiagnosticLog",
    "Write-JCodeMunchAtomicText",
    "Get-JCodeMunchSecurityDescriptor",
    "Set-JCodeMunchSecurityDescriptor",
    "Test-JCodeMunchSecurityDescriptorEquivalent",
    "Write-JCodeMunchStateDocument",
    "Read-JCodeMunchStateDocument",
    "New-JCodeMunchSecret",
    "Get-JCodeMunchSecret",
    "Enter-JCodeMunchStateLock",
    "Exit-JCodeMunchStateLock",
    "Invoke-JCodeMunchStateLock"
)

# The public lifecycle command surface is implemented by the JSON wrapper:
# configure, acquire, release, heartbeat, status, stop, clean, rollback.
