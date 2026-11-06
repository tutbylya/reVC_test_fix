# Source-only helpers: inspect the generated solution rather than assuming
# Premake's logical x86 label is also MSBuild's platform (normally Win32).
function Get-ModLoaderArchitecture {
    param([string]$Platform)
    switch -Regex ($Platform) {
        '^(Win32|x86)$' { return 'x86' }
        '^x64$' { return 'x64' }
        default { return '' }
    }
}

function Test-ModLoaderReleaseName {
    param([string]$Name, [string]$Architecture)
    if ($Name -match '^Release$') { return $true }
    if ($Name -match '^Release\s+(Win32|x86|x64)$') {
        return (Get-ModLoaderArchitecture $Matches[1]) -eq $Architecture
    }
    return $false
}

function Get-ModLoaderSolutionConfiguration {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SolutionText,
        [Parameter(Mandatory = $true)][ValidateSet('x86', 'x64')][string]$Architecture
    )
    $section = ''
    $seenSections = @{}
    $pairs = [System.Collections.Generic.List[object]]::new()
    $projects = [System.Collections.Generic.List[string]]::new()
    $mappings = @{}
    foreach ($rawLine in ($SolutionText -split "`n")) {
        $line = $rawLine.Trim()
        if ($line -match '^Project\("[^"]+"\)\s*=\s*"[^"]+",\s*"[^"\r\n]+\.vcxproj",\s*"(\{[0-9A-Fa-f-]{36}\})"$') {
            if ($projects.Contains($Matches[1])) { throw 'Duplicate C++ project in the generated solution.' }
            $projects.Add($Matches[1])
        }
        if ($line -match '^GlobalSection\((SolutionConfigurationPlatforms|ProjectConfigurationPlatforms)\)\s*=\s*(preSolution|postSolution)$') {
            $section = $Matches[1]
            if ($seenSections.ContainsKey($section)) { throw "Duplicate $section section in the generated solution." }
            $seenSections[$section] = $true
            continue
        }
        if ($line -eq 'EndGlobalSection') { $section = ''; continue }
        if ($section -eq 'SolutionConfigurationPlatforms' -and $line) {
            if ($line -notmatch '^([^|=]+)\|([^|=]+)\s*=\s*([^=]+)$') {
                throw "Invalid generated solution configuration: $line"
            }
            $configuration = $Matches[1].Trim()
            $platform = $Matches[2].Trim()
            $pair = "$configuration|$platform"
            if ($Matches[3].Trim() -cne $pair) { throw "Inconsistent generated solution configuration: $line" }
            if (@($pairs | Where-Object { $_.Pair -eq $pair }).Count) { throw "Duplicate solution configuration: $pair" }
            $pairs.Add([PSCustomObject]@{ Configuration = $configuration; Platform = $platform; Pair = $pair })
        } elseif ($section -eq 'ProjectConfigurationPlatforms' -and
                  $line -match '^(\{[0-9A-Fa-f-]{36}\})\.(.+)\.(ActiveCfg|Build\.0)\s*=\s*(.+)$') {
            $key = "$($Matches[1]).$($Matches[2].Trim()).$($Matches[3])"
            if ($mappings.ContainsKey($key)) { throw "Duplicate project configuration: $key" }
            $mappings[$key] = $Matches[4].Trim()
        }
    }
    if (!$projects.Count -or !$seenSections.ContainsKey('SolutionConfigurationPlatforms') -or
        !$seenSections.ContainsKey('ProjectConfigurationPlatforms')) {
        throw 'Generated solution is missing its C++ projects or configuration sections.'
    }
    $candidates = @($pairs | Where-Object {
        (Get-ModLoaderArchitecture $_.Platform) -eq $Architecture -and
        (Test-ModLoaderReleaseName $_.Configuration $Architecture)
    })
    if ($candidates.Count -ne 1) {
        throw "Expected exactly one Release solution configuration for $Architecture; found $($candidates.Count). Available: $($pairs.Pair -join ', ')"
    }
    $selected = $candidates[0]
    foreach ($project in $projects) {
        $activeKey = "$project.$($selected.Pair).ActiveCfg"
        $buildKey = "$project.$($selected.Pair).Build.0"
        if (!$mappings.ContainsKey($activeKey) -or !$mappings.ContainsKey($buildKey) -or
            $mappings[$activeKey] -cne $mappings[$buildKey]) {
            throw "Project $project is missing a consistent build mapping for $($selected.Pair)."
        }
        $target = $mappings[$activeKey]
        if ($target -notmatch '^([^|]+)\|([^|]+)$') { throw "Invalid project configuration: $target" }
        $targetConfiguration = $Matches[1].Trim()
        $targetPlatform = $Matches[2].Trim()
        if (!(Test-ModLoaderReleaseName $targetConfiguration $Architecture) -or
            (Get-ModLoaderArchitecture $targetPlatform) -ne $Architecture -or
            $targetConfiguration -cne $selected.Configuration -or
            $targetPlatform -cne $selected.Platform) {
            # A differently named solution Platform can leak into project-level
            # global properties. Do not silently build the wrong native target.
            throw "Project $project has an incompatible mapping for $($selected.Pair): $target"
        }
    }
    return $selected
}

function Write-ModLoaderBuildProperties {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Configuration
    )
    # The values come from the strictly checked solution names above. A response
    # file lets MSBuild parse its own quotes identically under PowerShell 5/7.
    foreach ($value in @($Configuration.Configuration, $Configuration.Platform)) {
        if ($value -match '["\r\n]') { throw 'Invalid MSBuild property value.' }
    }
    $lines = @(('/p:Configuration="' + $Configuration.Configuration + '"'),
               ('/p:Platform="' + $Configuration.Platform + '"'))
    [IO.File]::WriteAllLines($Path, [string[]]$lines, [Text.UTF8Encoding]::new($false))
}
