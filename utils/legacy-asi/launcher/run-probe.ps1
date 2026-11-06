[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
$taskOutput = Join-Path $taskRoot 'build\legacy-asi-launcher-probe'
$taskFixture = Join-Path $taskOutput 'legacy-asi\identity_fixture.asi'
$taskLauncher = Join-Path $taskOutput 'launcher.exe'
$taskHash = (Get-FileHash -LiteralPath $taskFixture -Algorithm SHA256).Hash
& $taskLauncher
$taskExit = $LASTEXITCODE
$taskAfter = (Get-FileHash -LiteralPath $taskFixture -Algorithm SHA256).Hash
if ($taskAfter -ne $taskHash) { throw 'The source fixture file changed; only in-memory TLS/IAT data may be modified.' }
if ($taskExit -ne 0) { throw "The owned-child launch probe failed with exit $taskExit. No retry is performed." }
Write-Host "Owned-child launch/TLS fixture passed; ASI SHA-256 remained $taskHash."
