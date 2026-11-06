[CmdletBinding()]
param([switch]$Run)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
$taskOutput = Join-Path $taskRoot ('build\legacy-asi-tests\loader-build-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path (Join-Path $taskOutput 'loader-objects') | Out-Null

$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $taskVsWhere)) { throw 'Visual Studio vswhere.exe was not found.' }
$taskVisualStudio = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVisualStudio) { throw 'Visual Studio C++ x86 build tools were not found.' }
$taskVcVars = Join-Path $taskVisualStudio 'VC\Auxiliary\Build\vcvars32.bat'

Push-Location -LiteralPath $taskRoot
try {
    # Use a child developer environment; do not mutate the caller's PATH or
    # generate/overwrite the game's normal solution or binaries.
    # The opt-in canonical test PE owns the ABI range before Windows startup
    # allocations, exactly like the production bootstrap. Normal CRT entry and
    # compiler protections remain enabled; no OS mitigation policy is changed.
    $taskCompileSteps = @(
        ('call "{0}" >nul' -f $taskVcVars)
        ('cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /DREVC_LEGACY_ASI /DREVC_LEGACY_ASI_CORE_DLL /Isrc\extras utils\legacy-asi\tests\loader_tests.cpp utils\legacy-asi\tests\canonical_test_arena.cpp src\extras\legacy_asi_memory.cpp src\extras\legacy_asi_entry.cpp src\extras\legacy_asi_loader.cpp src\extras\legacy_asi_identity.cpp /Fo"{0}\loader-objects\\" /Fe"{0}\loader_tests.exe" /link /BASE:0x400000 /FIXED /DYNAMICBASE:NO /INCREMENTAL:NO /SUBSYSTEM:CONSOLE' -f $taskOutput)
        ('cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DFIXTURE_ID=1 utils\legacy-asi\tests\fixture.cpp /Fo"{0}\loader-objects\fixture_one.obj" /Fe"{0}\fixture_one.asi" /link /IMPLIB:"{0}\fixture_one.lib"' -f $taskOutput)
        ('cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DFIXTURE_ID=2 utils\legacy-asi\tests\fixture.cpp /Fo"{0}\loader-objects\fixture_two.obj" /Fe"{0}\fixture_two.asi" /link /IMPLIB:"{0}\fixture_two.lib"' -f $taskOutput)
        ('cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DLOADER_FAILURE_FIXTURE utils\legacy-asi\tests\loader_tests.cpp /Fo"{0}\loader-objects\loader_failure.obj" /Fe"{0}\loader_failure.asi" /link /IMPLIB:"{0}\loader_failure.lib"' -f $taskOutput)
    )
    $taskCompile = $taskCompileSteps -join ' && '
    & $env:ComSpec /d /c $taskCompile
    if ($LASTEXITCODE -ne 0) { throw "Loader fixture compilation failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
if (-not $Run) {
    Write-Host 'Built only. Pass -Run to execute locally built source fixtures in isolated test processes.'
    return
}

$taskCases = Join-Path $taskOutput ('cases\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $taskCases | Out-Null
$taskOne = Join-Path $taskOutput 'fixture_one.asi'
$taskTwo = Join-Path $taskOutput 'fixture_two.asi'
$taskFailure = Join-Path $taskOutput 'loader_failure.asi'
$taskHashOne = (Get-FileHash -LiteralPath $taskOne -Algorithm SHA256).Hash
$taskHashTwo = (Get-FileHash -LiteralPath $taskTwo -Algorithm SHA256).Hash
$taskHashFailure = (Get-FileHash -LiteralPath $taskFailure -Algorithm SHA256).Hash
$taskCasesPassed = 0
$taskCaseRecords = [System.Collections.Generic.List[object]]::new()
$taskUtf8 = [System.Text.UTF8Encoding]::new($false)
$taskReport = Join-Path $taskCases 'results.json'

function Save-LoaderReport {
    # Keep completed negative results even if a later execution case is blocked
    # by an address collision. A blocked/failed case is never reported as passed.
    $taskJson = $taskCaseRecords | ConvertTo-Json -Depth 3
    [IO.File]::WriteAllText($taskReport, $taskJson, $taskUtf8)
}

function New-LoaderCase([string]$Name, [switch]$Second, [switch]$Failing) {
    $taskCase = Join-Path $taskCases $Name
    $taskModules = Join-Path $taskCase 'legacy-asi'
    New-Item -ItemType Directory -Force -Path $taskModules | Out-Null
    if ($Failing) {
        Copy-Item -LiteralPath $taskFailure -Destination (Join-Path $taskModules 'loader_failure.asi')
    } else {
        Copy-Item -LiteralPath $taskOne -Destination (Join-Path $taskModules 'fixture_one.asi')
        if ($Second) { Copy-Item -LiteralPath $taskTwo -Destination (Join-Path $taskModules 'fixture_two.asi') }
    }
    return $taskCase
}

function Set-Manifest([string]$Directory, [string]$Contents) {
    [IO.File]::WriteAllText((Join-Path $Directory 'legacy-asi\enabled.ini'), $Contents, $taskUtf8)
}

function Test-LoaderCase([string]$Directory, [bool]$Success, [int]$Count = 0, [bool]$Execution = $false) {
    $taskBefore = @{}
    foreach ($taskFile in Get-ChildItem -LiteralPath (Join-Path $Directory 'legacy-asi') -Filter '*.asi' -File) {
        $taskBefore[$taskFile.Name] = (Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash
    }
    Write-Host ("Testing {0}" -f (Split-Path -Leaf $Directory))
    & (Join-Path $taskOutput 'loader_tests.exe') $Directory ([int]$Success) $Count ([int]$Execution)
    if ($LASTEXITCODE -ne 0) {
        $taskCaseRecords.Add([PSCustomObject]@{ Case = (Split-Path -Leaf $Directory); ExpectedSuccess = $Success; ExpectedCount = $Count; ExpectedExecution = $Execution; Passed = $false; ExitCode = $LASTEXITCODE })
        Save-LoaderReport
        throw "Loader regression failed: $Directory (exit $LASTEXITCODE). No ASLR retry is performed. Partial report: $taskReport"
    }
    foreach ($taskFile in Get-ChildItem -LiteralPath (Join-Path $Directory 'legacy-asi') -Filter '*.asi' -File) {
        if ((Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash -ne $taskBefore[$taskFile.Name]) {
            throw "The loader changed fixture bytes: $($taskFile.FullName)"
        }
    }
    $script:taskCasesPassed++
    $taskCaseRecords.Add([PSCustomObject]@{ Case = (Split-Path -Leaf $Directory); ExpectedSuccess = $Success; ExpectedCount = $Count; ExpectedExecution = $Execution; Passed = $true })
    Save-LoaderReport
}

$taskCase = New-LoaderCase 'missing-manifest'
Test-LoaderCase $taskCase $true
$taskCase = New-LoaderCase 'empty-manifest'
Set-Manifest $taskCase ''
Test-LoaderCase $taskCase $true
$taskCase = New-LoaderCase 'empty-section'
Set-Manifest $taskCase "[Modules]`r`n; None enabled.`r`n"
Test-LoaderCase $taskCase $true

$taskInvalidManifests = [ordered]@{
    'wrong-section' = "[Plugins]`nfixture_one.asi=$taskHashOne`n"
    'missing-section' = "fixture_one.asi=$taskHashOne`n"
    'duplicate-section' = "[Modules]`n[modules]`n"
    'empty-value' = "[Modules]`nfixture_one.asi=`n"
    'truncated-hash' = "[Modules]`nfixture_one.asi=$($taskHashOne.Substring(0, 63))`n"
    'invalid-hex' = "[Modules]`nfixture_one.asi=$('Z' * 64)`n"
    'mismatched-hash' = "[Modules]`nfixture_one.asi=$('0' * 64)`n"
    'duplicate-case-insensitive-name' = "[Modules]`nfixture_one.asi=$taskHashOne`nFIXTURE_ONE.ASI=$taskHashOne`n"
    'parent-path' = "[Modules]`n..\fixture_one.asi=$taskHashOne`n"
    'slash-path' = "[Modules]`nsub/fixture_one.asi=$taskHashOne`n"
    'colon-path' = "[Modules]`nC:fixture_one.asi=$taskHashOne`n"
    'control-character' = "[Modules]`nfixture`t_one.asi=$taskHashOne`n"
    'no-equals' = "[Modules]`nfixture_one.asi`n"
    'missing-module' = "[Modules]`nabsent.asi=$taskHashOne`n"
    'oversized-manifest' = "[Modules]`n;" + ('x' * 65536)
}
foreach ($taskEntry in $taskInvalidManifests.GetEnumerator()) {
    $taskCase = New-LoaderCase $taskEntry.Key
    Set-Manifest $taskCase $taskEntry.Value
    Test-LoaderCase $taskCase $false
}

$taskCase = New-LoaderCase 'too-many-modules'
$taskManifest = "[Modules]`n"
for ($taskIndex = 0; $taskIndex -lt 33; ++$taskIndex) { $taskManifest += "module$taskIndex.asi=$taskHashOne`n" }
Set-Manifest $taskCase $taskManifest
Test-LoaderCase $taskCase $false

$taskCase = New-LoaderCase 'nul-utf16-manifest'
$taskUtf16 = [System.Text.UnicodeEncoding]::new($false, $true)
$taskManifestPath = Join-Path $taskCase 'legacy-asi\enabled.ini'
[IO.File]::WriteAllBytes($taskManifestPath, [byte[]]($taskUtf16.GetPreamble() + $taskUtf16.GetBytes("[Modules]`nfixture_one.asi=$taskHashOne`n")))
Test-LoaderCase $taskCase $false

$taskCase = New-LoaderCase 'invalid-utf8-filename'
$taskManifestBytes = [System.Text.Encoding]::ASCII.GetBytes("[Modules]`nfixture") + [byte]0xFF + [System.Text.Encoding]::ASCII.GetBytes("_one.asi=$taskHashOne`n")
[IO.File]::WriteAllBytes((Join-Path $taskCase 'legacy-asi\enabled.ini'), [byte[]]$taskManifestBytes)
Test-LoaderCase $taskCase $false

foreach ($taskPeCase in @('wrong-architecture', 'not-a-dll', 'truncated-pe')) {
    $taskCase = New-LoaderCase $taskPeCase
    $taskImage = Join-Path $taskCase 'legacy-asi\fixture_one.asi'
    $taskBytes = [IO.File]::ReadAllBytes($taskImage)
    $taskNt = [BitConverter]::ToInt32($taskBytes, 0x3C)
    switch ($taskPeCase) {
        'wrong-architecture' { $taskBytes[$taskNt + 4] = 0x64; $taskBytes[$taskNt + 5] = 0x86 }
        'not-a-dll' { $taskBytes[$taskNt + 23] = $taskBytes[$taskNt + 23] -band 0xDF }
        'truncated-pe' { $taskBytes = [byte[]]$taskBytes[0..39] }
    }
    [IO.File]::WriteAllBytes($taskImage, $taskBytes)
    $taskMutatedHash = (Get-FileHash -LiteralPath $taskImage -Algorithm SHA256).Hash
    Set-Manifest $taskCase "[Modules]`nfixture_one.asi=$taskMutatedHash`n"
    Test-LoaderCase $taskCase $false
}

$taskCase = New-LoaderCase 'valid-first-bad-second' -Second
Set-Manifest $taskCase "[Modules]`nfixture_one.asi=$taskHashOne`nfixture_two.asi=$('0' * 64)`n"
Test-LoaderCase $taskCase $false

$taskCase = New-LoaderCase 'single-fixture'
Set-Manifest $taskCase "[Modules]`nfixture_one.asi=$taskHashOne`n"
Test-LoaderCase $taskCase $true 1 $true
$taskCase = New-LoaderCase 'two-chained-fixtures' -Second
Set-Manifest $taskCase "[Modules]`nfixture_one.asi=$taskHashOne`nfixture_two.asi=$taskHashTwo`n"
Test-LoaderCase $taskCase $true 2 $true
$taskCase = New-LoaderCase 'first-dllmain-executed-then-failed' -Failing
Set-Manifest $taskCase "[Modules]`nloader_failure.asi=$taskHashFailure`n"
Test-LoaderCase $taskCase $false 0 $true
$taskCase = New-LoaderCase 'loaded-first-then-rejecting-dllmain' -Failing
Copy-Item -LiteralPath $taskOne -Destination (Join-Path $taskCase 'legacy-asi\fixture_one.asi')
Set-Manifest $taskCase "[Modules]`nfixture_one.asi=$taskHashOne`nloader_failure.asi=$taskHashFailure`n"
Test-LoaderCase $taskCase $false 1 $true

if ((Get-FileHash -LiteralPath $taskOne -Algorithm SHA256).Hash -ne $taskHashOne -or
    (Get-FileHash -LiteralPath $taskTwo -Algorithm SHA256).Hash -ne $taskHashTwo -or
    (Get-FileHash -LiteralPath $taskFailure -Algorithm SHA256).Hash -ne $taskHashFailure) {
    throw 'A built source fixture changed during regression testing.'
}
Save-LoaderReport
Write-Host "Legacy ASI loader: $taskCasesPassed isolated process cases passed. Report: $taskReport"
Write-Host 'Only locally built fixed-address test fixtures were executed; this does not establish third-party mod compatibility.'
