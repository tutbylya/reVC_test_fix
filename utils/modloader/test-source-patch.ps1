#Requires -Version 5.1
[CmdletBinding()]
param([string]$SourceRepository)

# No network, compiler, game, DLL or ASI execution. All mutations below belong
# to newly created, disposable source fixtures, never the supplied repository.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$SourceRepository) { $SourceRepository = Join-Path $repositoryRoot 'build/modloader-upstream' }
$SourceRepository = (Resolve-Path -LiteralPath $SourceRepository).Path
$commit = '76c127e983069bcb1308b198c11ce8b81147b0bc'
$realBuild = Join-Path $PSScriptRoot 'build.ps1'
$originalPatch = Join-Path $PSScriptRoot 'revc-compat.patch'
$originalHash = (Get-FileHash -LiteralPath $originalPatch -Algorithm SHA256).Hash
$patchLf = [IO.File]::ReadAllText($originalPatch).Replace("`r`n", "`n")
$expectedDiff = $patchLf.TrimEnd([char[]]"`r`n")
$runRoot = Join-Path $repositoryRoot ('build/modloader-source-tests/' + [Guid]::NewGuid().ToString('N'))
$runner = (Get-Process -Id $PID).Path
$checks = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
    $script:checks++
}

function Git-Text([string]$Directory, [string[]]$Arguments) {
    $lines = @(& git -C $Directory @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Git inspection failed in $Directory" }
    return ($lines -join "`n").Replace("`r`n", "`n").TrimEnd([char[]]"`r`n")
}

function New-Fixture([string]$Name, [bool]$AutoCrlf, [bool]$PatchCrlf) {
    $directory = Join-Path $runRoot $Name
    if (Test-Path -LiteralPath $directory) { throw "Fixture already exists: $directory" }
    & git -c core.longpaths=true clone --quiet --no-checkout --no-hardlinks $SourceRepository $directory
    if ($LASTEXITCODE -ne 0) { throw 'Fresh local fixture clone failed.' }
    & git -C $directory config core.longpaths true
    if ($LASTEXITCODE -ne 0) { throw 'Fixture path configuration failed.' }
    & git -C $directory config core.autocrlf $(if ($AutoCrlf) { 'true' } else { 'false' })
    if ($LASTEXITCODE -ne 0) { throw 'Fixture EOL configuration failed.' }
    & git -C $directory checkout --quiet --detach $commit
    if ($LASTEXITCODE -ne 0) { throw 'Fixture pinned checkout failed.' }
    $harness = Join-Path $runRoot ('h-' + $Name + '/utils/modloader')
    New-Item -ItemType Directory -Path $harness -Force | Out-Null
    Copy-Item -LiteralPath $realBuild -Destination (Join-Path $harness 'build.ps1')
    $text = if ($PatchCrlf) { $patchLf.Replace("`n", "`r`n") } else { $patchLf }
    $patch = Join-Path $harness 'revc-compat.patch'
    [IO.File]::WriteAllText($patch, $text, [Text.UTF8Encoding]::new($false))
    return [pscustomobject]@{ Directory = $directory; Build = (Join-Path $harness 'build.ps1'); Patch = $patch }
}

function Invoke-RealBuild($Fixture) {
    # Invoke the real copied script in an independent host so expected throws
    # become an inspectable exit status. Preserve the host's normal execution
    # policy; if it forbids the script, report that failure without overriding it.
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = @(& $runner -NoProfile -File $Fixture.Build -PrepareOnly -Architecture All `
            -SourceDirectory $Fixture.Directory -MSBuildPath (Join-Path $runRoot 'missing-msbuild.exe') 2>&1 | ForEach-Object { $_.ToString() })
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    return [pscustomobject]@{ Exit = $code; Text = ($lines -join "`n") }
}

function Snapshot($Fixture, [string]$ExtraFile = '') {
    $tracked = Git-Text $Fixture.Directory @('diff', '--binary', 'HEAD', '--')
    $staged = Git-Text $Fixture.Directory @('diff', '--cached', '--binary', 'HEAD', '--')
    $status = Git-Text $Fixture.Directory @('status', '--porcelain=v1', '--untracked-files=all')
    $indexHash = (Get-FileHash -LiteralPath (Join-Path $Fixture.Directory '.git/index') -Algorithm SHA256).Hash
    $configHash = (Get-FileHash -LiteralPath (Join-Path $Fixture.Directory '.git/config') -Algorithm SHA256).Hash
    $paths = @('include/modloader/modloader_reVC.h', 'src/core/folder.cpp', 'src/plugins/gta3/std.stream/backend.cpp', 'src/translator/gta3/vc/reVC.hpp')
    if ($ExtraFile) { $paths += $ExtraFile }
    $hashes = @($paths | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $Fixture.Directory $_) -Algorithm SHA256).Hash })
    return ($tracked, $staged, $status, $indexHash, $configHash, ($hashes -join ',')) -join "`n---snapshot---`n"
}

function Prepare($Fixture) {
    $result = Invoke-RealBuild $Fixture
    Assert-True ($result.Exit -eq 0) ("PrepareOnly failed: " + $result.Text)
    Assert-True ((Git-Text $Fixture.Directory @('diff', '--binary', 'HEAD', '--')) -ceq $expectedDiff) 'Prepared source does not match the exact compatibility patch.'
    foreach ($directory in @('build-revc', 'bin-revc', 'obj-revc')) {
        Assert-True (!(Test-Path -LiteralPath (Join-Path $Fixture.Directory $directory))) 'PrepareOnly entered the generation/build stage.'
    }
}

try {
    & git -C $SourceRepository cat-file -e ($commit + '^{commit}')
    if ($LASTEXITCODE -ne 0) { throw 'SourceRepository does not contain the pinned commit; this test never fetches from the network.' }
    New-Item -ItemType Directory -Path $runRoot | Out-Null
    Write-Host "Source-only patch fixtures: $runRoot"
    foreach ($auto in @($false, $true)) {
        foreach ($crlf in @($false, $true)) {
            $name = 'a' + [int]$auto + 'p' + [int]$crlf
            $fixture = New-Fixture $name $auto $crlf
            $patchHash = (Get-FileHash -LiteralPath $fixture.Patch -Algorithm SHA256).Hash
            Prepare $fixture
            $before = Snapshot $fixture
            Prepare $fixture
            Assert-True ((Snapshot $fixture) -ceq $before) 'A second preparation changed the source/index/config.'
            Assert-True ((Get-FileHash -LiteralPath $fixture.Patch -Algorithm SHA256).Hash -ceq $patchHash) 'The build rewrote the original fixture patch.'
            Write-Host "PASS autocrlf=$auto patchCRLF=$crlf; exact patch, unchanged second run."
        }
    }
    foreach ($kind in @('tracked', 'staged', 'untracked')) {
        $fixture = New-Fixture ('foreign-' + $kind) $true $true
        Prepare $fixture
        $extra = ''
        if ($kind -eq 'untracked') {
            $extra = 'foreign-source-note.txt'
            [IO.File]::WriteAllText((Join-Path $fixture.Directory $extra), "Foreign fixture content; must remain untouched.`n", [Text.UTF8Encoding]::new($false))
        } else {
            $foreign = Join-Path $fixture.Directory 'src/core/folder.cpp'
            [IO.File]::AppendAllText($foreign, "`n// Foreign fixture edit; must not be cleaned or reset.`n", [Text.UTF8Encoding]::new($false))
            if ($kind -eq 'staged') {
                & git -C $fixture.Directory add -- src/core/folder.cpp
                if ($LASTEXITCODE -ne 0) { throw 'Staging the owned test edit failed.' }
            }
        }
        $before = Snapshot $fixture $extra
        $result = Invoke-RealBuild $fixture
        Assert-True ($result.Exit -ne 0) "Foreign $kind changes were accepted."
        $expectedMessage = if ($kind -eq 'untracked') { 'Unexpected untracked source file' } else { 'Source changes do not exactly match' }
        Assert-True ($result.Text.Contains($expectedMessage)) ("Foreign $kind rejection failed for an unrelated reason: " + $result.Text)
        Assert-True ((Snapshot $fixture $extra) -ceq $before) "Rejected $kind changes/index/config were modified."
        Write-Host "PASS foreign $kind changes rejected and preserved."
    }
    Assert-True ((Get-FileHash -LiteralPath $originalPatch -Algorithm SHA256).Hash -ceq $originalHash) 'The production compatibility patch changed.'
    Write-Host "Source preparation regression passed: $checks checks. All fixtures retained in $runRoot"
} catch {
    Write-Host "Source preparation regression FAILED. Fixtures retained in $runRoot"
    throw
} finally {
    if ((Get-FileHash -LiteralPath $originalPatch -Algorithm SHA256).Hash -cne $originalHash) {
        throw 'The production compatibility patch changed during the test.'
    }
}
