#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'All')][string]$Architecture = 'x64',
    [switch]$PackageOnly,
    [switch]$PrepareOnly,
    [string]$SourceDirectory,
    [string]$OutputDirectory,
    [string]$InstallDirectory,
    [string]$MSBuildPath,
    [ValidateRange(1, 64)][int]$Jobs = 2
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This helper builds Windows Mod Loader binaries only.' }
if ($PrepareOnly -and ($PackageOnly -or $InstallDirectory)) {
    throw 'PrepareOnly cannot be combined with PackageOnly or InstallDirectory.'
}
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$sourceUrl = 'https://github.com/Cowboy-69/modloader.git'
$sourceCommit = '76c127e983069bcb1308b198c11ce8b81147b0bc'
$compatPatch = Join-Path $PSScriptRoot 'revc-compat.patch'
$premake = Join-Path $repositoryRoot 'premake5.exe'
$plugins = @('std.fx', 'std.stream', 'std.data', 'std.text', 'std.scm', 'std.sprites', 'std.movies')
if (!$SourceDirectory) { $SourceDirectory = Join-Path $repositoryRoot 'build/modloader-upstream' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repositoryRoot 'build/modloader-dist' }
$SourceDirectory = [IO.Path]::GetFullPath($SourceDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$architectures = @($Architecture)
if ($Architecture -eq 'All') { $architectures = @('x64', 'x86') }
if ($InstallDirectory -and $architectures.Count -ne 1) {
    throw 'Install one architecture at a time; do not mix x86 and x64 in one game directory.'
}

function Invoke-Checked {
    param([string]$Program, [string[]]$NativeArguments)
    & $Program @NativeArguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)." }
}

function Get-GitText {
    param([string[]]$GitArguments)
    $result = & git @GitArguments
    if ($LASTEXITCODE -ne 0) { throw 'Git inspection failed.' }
    return (($result -join "`n").Replace("`r`n", "`n").TrimEnd([char[]]"`r`n"))
}

function Get-PeMachine {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE image: $Path" }
        $stream.Position = 0x3C
        $stream.Position = $reader.ReadUInt32()
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE header: $Path" }
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Assert-PluginDirectory {
    param([string]$Directory)
    if (!(Test-Path -LiteralPath $Directory)) { return }
    $allowed = @($plugins | ForEach-Object { "$_.dll" })
    $unexpected = @(Get-ChildItem -LiteralPath $Directory -File -Filter '*.dll' -Force |
        Where-Object { $_.Name -notin $allowed })
    if ($unexpected.Count) {
        throw "Unsupported existing plugin DLLs in ${Directory}: $($unexpected.Name -join ', '). No files were deleted."
    }
}

function Find-MSBuild {
    if ($MSBuildPath) {
        if (!(Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) { throw "MSBuild not found: $MSBuildPath" }
        return [IO.Path]::GetFullPath($MSBuildPath)
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 C++ Build Tools and a Windows SDK.' }
    $installation = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'Visual Studio 2022 with C++ x86/x64 tools was not found.' }
    $found = Join-Path ($installation | Select-Object -First 1) 'MSBuild/Current/Bin/MSBuild.exe'
    if (!(Test-Path -LiteralPath $found)) { throw "MSBuild not found: $found" }
    return $found
}

if (!(Test-Path -LiteralPath (Join-Path $SourceDirectory '.git'))) {
    if ($PackageOnly) { throw "No existing source build: $SourceDirectory" }
    if (Test-Path -LiteralPath $SourceDirectory) { throw 'Source directory exists but is not a Git checkout; refusing to overwrite it.' }
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($SourceDirectory)) -Force | Out-Null
    Invoke-Checked git @('clone', '--no-checkout', $sourceUrl, $SourceDirectory)
    Invoke-Checked git @('-C', $SourceDirectory, 'checkout', '--detach', $sourceCommit)
}
if ((Get-GitText @('-C', $SourceDirectory, 'rev-parse', 'HEAD')) -cne $sourceCommit) {
    throw "Expected source commit $sourceCommit. Existing checkout was not reset or changed."
}
$untracked = @(Get-GitText @('-C', $SourceDirectory, 'ls-files', '--others', '--exclude-standard'))
foreach ($line in ($untracked -split "`n")) {
    if ($line -and $line -notmatch '^(bin-revc/|obj-revc/|build-revc/|revc-premake\.lua$|revc-test-fix\.premake\.lua$)') {
        throw "Unexpected untracked source file: $line. Preserve/move your changes before building."
    }
}
$normalizedPatch = [IO.File]::ReadAllText($compatPatch).Replace("`r`n", "`n")
$expectedDiff = $normalizedPatch.TrimEnd([char[]]"`r`n")
$actualDiff = Get-GitText @('-C', $SourceDirectory, 'diff', '--binary', 'HEAD', '--')
if (!$actualDiff -and !$PackageOnly) {
    # Windows checkout/manual copies can turn the patch itself into CRLF.
    # For an unterminated source line, Git's "No newline" marker removes only
    # the patch LF: a converted patch leaves a spurious CR in the match.
    # Normalize a private copy, not source files or the user's checkout/config.
    $temporaryPatch = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($temporaryPatch, $normalizedPatch, [Text.UTF8Encoding]::new($false))
        Invoke-Checked git @('-C', $SourceDirectory, 'apply', '--check', $temporaryPatch)
        Invoke-Checked git @('-C', $SourceDirectory, 'apply', $temporaryPatch)
    } finally {
        Remove-Item -LiteralPath $temporaryPatch -ErrorAction Stop
    }
    $actualDiff = Get-GitText @('-C', $SourceDirectory, 'diff', '--binary', 'HEAD', '--')
}
if ($actualDiff -cne $expectedDiff) {
    throw 'Source changes do not exactly match revc-compat.patch. No reset/cleanup was performed.'
}
if ($PrepareOnly) {
    Write-Output "Pinned Mod Loader sources and compatibility patch verified: $SourceDirectory"
    return
}

$mappedDrive = $null
$buildTool = $null
try {
    if (!$PackageOnly) {
        if (!(Test-Path -LiteralPath $premake -PathType Leaf)) { throw 'Bundled premake5.exe is missing from the game repository root.' }
        $buildTool = Find-MSBuild
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'revc-premake.lua') -Destination (Join-Path $SourceDirectory 'revc-test-fix.premake.lua') -Force
        # Premake's narrow-character path handling needs an ASCII working path.
        # Never reuse M: (or any other pre-existing drive mapping).
        foreach ($letter in @('Z', 'Y', 'X', 'W', 'V', 'U', 'T', 'S', 'R', 'Q', 'P', 'O', 'N')) {
            if (Get-PSDrive -Name $letter -ErrorAction SilentlyContinue) { continue }
            $candidate = "$letter`:"
            & subst.exe $candidate $SourceDirectory
            if ($LASTEXITCODE -eq 0) { $mappedDrive = $candidate; break }
        }
        if (!$mappedDrive) { throw 'No free temporary drive letter (N: through Z:) for Unicode-safe Premake generation.' }
        Push-Location "$mappedDrive\"
        try {
            Invoke-Checked $premake @('--file=revc-test-fix.premake.lua', 'vs2022')
            foreach ($arch in $architectures) {
                Invoke-Checked $buildTool @('build-revc/modloader-revc.sln', "/m:$Jobs", '/nologo', '/verbosity:minimal', '/p:Configuration=Release', "/p:Platform=$arch")
            }
        } finally { Pop-Location }
    }

    foreach ($arch in $architectures) {
        $compiled = Join-Path $SourceDirectory "bin-revc/$arch/Release"
        $package = Join-Path $OutputDirectory $arch
        $data = Join-Path $package 'modloader/.data'
        $pluginDirectory = Join-Path $data 'plugins/gta3'
        Assert-PluginDirectory $pluginDirectory
        $machine = 0x8664
        if ($arch -eq 'x86') { $machine = 0x14C }
        $binaryNames = @('modloader.asi') + @($plugins | ForEach-Object { "$_.dll" })
        foreach ($name in $binaryNames) {
            $binary = Join-Path $compiled $name
            if (!(Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Build output missing: $binary" }
            if ((Get-PeMachine $binary) -ne $machine) { throw "Wrong binary architecture: $binary" }
        }
        New-Item -ItemType Directory -Path $pluginDirectory -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $compiled 'modloader.asi') -Destination $package -Force
        foreach ($name in $plugins) {
            Copy-Item -LiteralPath (Join-Path $compiled "$name.dll") -Destination $pluginDirectory -Force
        }
        Copy-Item -LiteralPath (Join-Path $SourceDirectory 'LICENSE') -Destination $data -Force
        foreach ($directory in @('licenses', 'text')) {
            Copy-Item -LiteralPath (Join-Path $SourceDirectory "doc/$directory") -Destination $data -Recurse -Force
        }
        $extraLicenses = @{
            'deps/injector/LICENSE' = 'injector.txt'
            'deps/cereal/include/cereal/external/rapidxml/license.txt' = 'rapidxml.txt'
            'deps/cereal/include/cereal/external/rapidjson/license.txt' = 'rapidjson.txt'
        }
        foreach ($relativePath in $extraLicenses.Keys) {
            Copy-Item -LiteralPath (Join-Path $SourceDirectory $relativePath) -Destination (Join-Path $data ('licenses/' + $extraLicenses[$relativePath])) -Force
        }
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $SourceDirectory 'doc/config') -File -Filter '*.ini.0') {
            Copy-Item -LiteralPath $file.FullName -Destination $data -Force
        }
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $SourceDirectory 'doc/readme') -File) {
            Copy-Item -LiteralPath $file.FullName -Destination $data -Force
        }
        foreach ($name in @('Profiles.md', 'Command Line Arguments.md', 'CHANGELOG.md')) {
            Copy-Item -LiteralPath (Join-Path $SourceDirectory "doc/$name") -Destination $data -Force
        }
        Copy-Item -LiteralPath $compatPatch -Destination $data -Force
        Copy-Item -LiteralPath (Join-Path $repositoryRoot 'docs/ModLoader.md') -Destination (Join-Path $data 'reVC-ModLoader.md') -Force
        $hashes = [ordered]@{}
        foreach ($name in $binaryNames) { $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $compiled $name) -Algorithm SHA256).Hash.ToLowerInvariant() }
        [ordered]@{
            source = $sourceUrl
            commit = $sourceCommit
            patchSha256 = (Get-FileHash -LiteralPath $compatPatch -Algorithm SHA256).Hash.ToLowerInvariant()
            architecture = $arch
            configuration = 'Release'
            packagedFromExistingBuild = [bool]$PackageOnly
            msbuild = $buildTool
            binariesSha256 = $hashes
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $data 'reVC-build.json') -Encoding UTF8
        Write-Output "Package ready: $package"

        if ($InstallDirectory) {
            $gameDirectory = [IO.Path]::GetFullPath($InstallDirectory)
            $gameExe = Join-Path $gameDirectory 'reVC.exe'
            if (!(Test-Path -LiteralPath $gameExe -PathType Leaf)) { throw 'Install directory must already contain reVC.exe.' }
            if ((Get-PeMachine $gameExe) -ne $machine) { throw 'reVC.exe and Mod Loader architecture do not match.' }
            Assert-PluginDirectory (Join-Path $gameDirectory 'modloader/.data/plugins/gta3')
            $running = @(Get-Process -Name reVC -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $gameExe })
            if ($running.Count) { throw 'Close this reVC instance before installing Mod Loader.' }
            foreach ($item in Get-ChildItem -LiteralPath $package -Force) {
                Copy-Item -LiteralPath $item.FullName -Destination $gameDirectory -Recurse -Force
            }
            Write-Output "Installed in: $gameDirectory (existing mods and active .ini files preserved)"
        }
    }
} finally {
    if ($mappedDrive) {
        & subst.exe $mappedDrive /D
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not remove the temporary mapping $mappedDrive" }
    }
}
