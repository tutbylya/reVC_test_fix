[CmdletBinding()]
param()

# Local, opt-in preparation only: no downloads, no game/mod execution, and no
# writes to an installed game. Game assets remain in the existing workspace copy.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$taskAssets = Join-Path $taskRoot 'build\modloader-game-test'
$taskBinary = Join-Path $taskRoot 'bin\win-x86-librw_d3d9-oal-legacy\Release'
$taskMod = Join-Path $taskRoot 'build\amazing-camera-mod'
$taskTest = Join-Path $taskRoot 'build\amazing-snapshot-test'
$taskExpected = '1274308B5A1EE936E3EC68AF8FC91034D2BD6DA5A6BFB99A77DDA7FA30A11B03'
if ((Get-FileHash -LiteralPath (Join-Path $taskMod 'AmazingSnapshot.VC.asi') -Algorithm SHA256).Hash -ne $taskExpected) {
    throw 'The local ASI is not the reviewed original v1.1 binary.'
}
if (-not (Test-Path -LiteralPath (Join-Path $taskAssets 'models\gta3.img'))) { throw 'Workspace asset copy missing.' }
New-Item -ItemType Directory -Force -Path $taskTest | Out-Null
foreach ($taskName in 'anim','Audio','data','models','movies','neo','TEXT','txd') {
    $taskSource = Join-Path $taskAssets $taskName
    $taskTarget = Join-Path $taskTest $taskName
    if (-not (Test-Path -LiteralPath $taskSource)) { continue }
    if (Test-Path -LiteralPath $taskTarget) {
        $taskItem = Get-Item -LiteralPath $taskTarget
        if ($taskItem.LinkType -ne 'Junction' -or $taskItem.Target -ne $taskSource) { throw "Unexpected asset target: $taskTarget" }
    } else { New-Item -ItemType Junction -Path $taskTarget -Target $taskSource | Out-Null }
}
foreach ($taskName in 'reVC.exe','reVC-game.exe','reVC-core.dll','reVC-core.pdb') {
    Copy-Item -LiteralPath (Join-Path $taskBinary $taskName) -Destination (Join-Path $taskTest $taskName) -Force
}
foreach ($taskName in 'openal-soft\dist\Win32\OpenAL32.dll','mpg123\dist\Win32\libmpg123-0.dll','libsndfile\dist\Win32\libsndfile-1.dll') {
    Copy-Item -LiteralPath (Join-Path $taskRoot ('vendor\' + $taskName)) -Destination $taskTest -Force
}
$taskModules = Join-Path $taskTest 'legacy-asi'
New-Item -ItemType Directory -Force -Path $taskModules | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $taskTest 'userfiles') | Out-Null
Copy-Item -LiteralPath (Join-Path $taskMod 'AmazingSnapshot.VC.asi') -Destination $taskModules -Force
Copy-Item -LiteralPath (Join-Path $taskMod 'AmazingSnapshot.ini') -Destination $taskModules -Force
$taskEncoding = [System.Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $taskModules 'enabled.ini'), "[Modules]`r`nAmazingSnapshot.VC.asi=$taskExpected`r`n", $taskEncoding)
if (-not (Test-Path -LiteralPath (Join-Path $taskTest 'reVC.ini'))) {
    [IO.File]::WriteAllText((Join-Path $taskTest 'reVC.ini'), @'
[VideoMode]
Width=960
Height=540
Depth=32
Windowed=1
Subsystem=0
[General]
Language=0
NoMovies=1
DrawVersionText=0
[Graphics]
FrameLimiter=1
MaxFPS=60
VSync=1
MultiSampling=0
[Audio]
MusicVolume=5
SfxVolume=5
'@, $taskEncoding)
}
Write-Host "Prepared isolated test: $taskTest"
Write-Host 'Start reVC.exe (launcher), not reVC-game.exe. ASI execution requires explicit consent.'
