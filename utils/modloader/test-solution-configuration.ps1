#Requires -Version 5.1
[CmdletBinding()]
param([string]$GeneratedSolution)

# Pure configuration parsing and response-file data. No compiler, DLL, ASI,
# Premake, subprocess, network access or user-source mutation is required.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'solution-configuration.ps1')
$checks = 0
$project = '{11111111-2222-3333-4444-555555555555}'

function Assert-True([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}

function New-Solution([string[]]$Pairs, [string[]]$Targets = @()) {
    if (!$Targets.Count) { $Targets = $Pairs }
    $lines = @('Microsoft Visual Studio Solution File, Format Version 12.00',
        ('Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "modloader", "modloader.vcxproj", "' + $project + '"'),
        'EndProject', 'Global', 'GlobalSection(SolutionConfigurationPlatforms) = preSolution')
    foreach ($pair in $Pairs) { $lines += "$pair = $pair" }
    $lines += 'EndGlobalSection', 'GlobalSection(ProjectConfigurationPlatforms) = postSolution'
    for ($index = 0; $index -lt $Pairs.Count; $index++) {
        $lines += "$project.$($Pairs[$index]).ActiveCfg = $($Targets[$index])"
        $lines += "$project.$($Pairs[$index]).Build.0 = $($Targets[$index])"
    }
    return ($lines + @('EndGlobalSection', 'EndGlobal')) -join "`n"
}

function Assert-Rejected([string]$Text, [string]$Architecture, [string]$Label) {
    $failed = $false
    try { Get-ModLoaderSolutionConfiguration -SolutionText $Text -Architecture $Architecture | Out-Null }
    catch { $failed = $true }
    Assert-True $failed "Accepted invalid solution: $Label"
}

$classic = New-Solution @('Debug|Win32', 'Debug|x64', 'Release|Win32', 'Release|x64')
$x86 = Get-ModLoaderSolutionConfiguration $classic x86
$x64 = Get-ModLoaderSolutionConfiguration $classic x64
Assert-True ($x86.Pair -ceq 'Release|Win32') 'x86 must use the generated Win32 solution platform, not logical x86.'
Assert-True ($x64.Pair -ceq 'Release|x64') 'x64 must keep the generated x64 platform.'
$combined = New-Solution @('Release x86|Win32', 'Release x64|x64')
Assert-True ((Get-ModLoaderSolutionConfiguration $combined x86).Pair -ceq 'Release x86|Win32') 'Combined x86 configuration was not preserved.'
Assert-True ((Get-ModLoaderSolutionConfiguration $combined x64).Pair -ceq 'Release x64|x64') 'Combined x64 configuration was not preserved.'
Assert-True ((Get-ModLoaderSolutionConfiguration ($combined.Replace("`n", "`r`n")) x86).Pair -ceq 'Release x86|Win32') 'CRLF solution parsing differs.'
Assert-Rejected (New-Solution @('Debug|Win32')) x86 'Debug is not Release'
Assert-Rejected (New-Solution @('Release|x64')) x86 'wrong architecture'
Assert-Rejected (New-Solution @('Release x64|Win32')) x86 'contradictory logical and native architecture'
Assert-Rejected (New-Solution @('Release|Win32', 'Release x86|Win32')) x86 'ambiguous Release choices'
Assert-Rejected (New-Solution @('Release|Win32', 'Release|Win32')) x86 'duplicate solution entry'
Assert-Rejected ($classic.Replace("$project.Release|Win32.Build.0 = Release|Win32", '')) x86 'missing build mapping'
Assert-Rejected ($classic.Replace("$project.Release|Win32.ActiveCfg = Release|Win32", "$project.Release|Win32.ActiveCfg = Release|x64")) x86 'mismatched ActiveCfg/Build.0'
Assert-Rejected (New-Solution @('Release|Win32') @('Release|x64')) x86 'wrong mapped project architecture'
Assert-Rejected (New-Solution @('Release|x86') @('Release|Win32')) x86 'logical platform would override physical project platform'
Assert-Rejected ($classic.Replace('GlobalSection(ProjectConfigurationPlatforms)', 'GlobalSection(NotProjectConfigurations)')) x86 'missing mapping section'
Assert-Rejected ($classic.Replace('Release|Win32 = Release|Win32', 'Release|Win32 = Release|x64')) x86 'inconsistent solution alias'

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture = Join-Path $root ('build/modloader-configuration-tests/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$response = Join-Path $fixture 'properties with spaces.rsp'
Write-ModLoaderBuildProperties -Path $response -Configuration (Get-ModLoaderSolutionConfiguration $combined x86)
$actualProperties = [IO.File]::ReadAllLines($response)
Assert-True ($actualProperties.Count -eq 2) 'MSBuild response must contain exactly two properties.'
Assert-True ($actualProperties[0] -ceq '/p:Configuration="Release x86"') 'Combined configuration must be quoted inside the response file.'
Assert-True ($actualProperties[1] -ceq '/p:Platform="Win32"') 'Response must select physical Win32, not logical x86.'
Assert-True (([IO.File]::ReadAllBytes($response))[0] -eq [byte][char]'/') 'Response should be UTF-8 without a BOM.'
if ($GeneratedSolution) {
    $text = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $GeneratedSolution).Path)
    foreach ($arch in @('x86', 'x64')) {
        $selection = Get-ModLoaderSolutionConfiguration $text $arch
        Assert-True ($selection.Configuration -match '^Release(?:\s+(Win32|x86|x64))?$') 'Generated configuration is not Release.'
        Write-Host "Generated solution: $arch -> $($selection.Pair)"
    }
}
Write-Host "Mod Loader solution selection passed: $checks checks. Response fixture retained in $fixture"
