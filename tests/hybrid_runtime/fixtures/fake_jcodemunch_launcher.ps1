#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Python,
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][int]$Port,
    [Parameter(Mandatory=$true)][string]$HostName,
    [Parameter(Mandatory=$true)][string]$ReadyFile,
    [Parameter(Mandatory=$true)][string]$IdentityFile,
    [Parameter(Mandatory=$true)][string]$StopFile
)

$process=Start-Process -FilePath $Python -ArgumentList @(
    $Fixture,
    "--port",[string]$Port,
    "--host",$HostName,
    "--ready-file",$ReadyFile,
    "--identity-file",$IdentityFile,
    "--stop-file",$StopFile
) -PassThru -WindowStyle Hidden
$process.WaitForExit()
exit $process.ExitCode
