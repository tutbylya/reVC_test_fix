@echo off
setlocal
pushd "%~dp0..\..\.."
if errorlevel 1 exit /b 1
if defined VSCMD_ARG_TGT_ARCH goto tools_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TASK_VSINSTALL=%%i"
if not defined TASK_VSINSTALL goto failed
call "%TASK_VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 goto failed
:tools_ready
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" goto failed
if not exist "build\legacy-asi-launcher-probe" mkdir "build\legacy-asi-launcher-probe"
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /DREVC_LAUNCHER_CONSOLE utils\legacy-asi\launcher\launcher.cpp utils\legacy-asi\launcher\debug_tls.cpp /Fobuild\legacy-asi-launcher-probe\ /Febuild\legacy-asi-launcher-probe\launcher.exe /link /BASE:0x20000000 /DYNAMICBASE /FIXED:NO
if errorlevel 1 goto failed
cl /nologo /c /utf-8 /std:c++20 /W4 /WX /MT /GS- /JMC- utils\legacy-asi\launcher\probe_entry.cpp /Fobuild\legacy-asi-launcher-probe\probe_entry.obj
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /DREVC_LEGACY_ASI utils\legacy-asi\launcher\probe.cpp src\extras\legacy_asi_identity.cpp build\legacy-asi-launcher-probe\probe_entry.obj /Fobuild\legacy-asi-launcher-probe\ /Febuild\legacy-asi-launcher-probe\reVC-game.exe /link /BASE:0x20000000 /DYNAMICBASE /FIXED:NO /ENTRY:ProbeEntry /IMPLIB:build\legacy-asi-launcher-probe\probe.lib
if errorlevel 1 goto failed
if not exist "build\legacy-asi-launcher-probe\legacy-asi" mkdir "build\legacy-asi-launcher-probe\legacy-asi"
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD utils\legacy-asi\launcher\identity_fixture.cpp /Fobuild\legacy-asi-launcher-probe\identity_fixture.obj /Febuild\legacy-asi-launcher-probe\legacy-asi\identity_fixture.asi /link /IMPLIB:build\legacy-asi-launcher-probe\identity_fixture.lib
if errorlevel 1 goto failed
if /i "%~1"=="--run" (
    powershell -NoProfile -File utils\legacy-asi\launcher\run-probe.ps1
    if errorlevel 1 goto failed
) else (
    echo Built only. Pass --run to execute only the local source probe.
)
popd
exit /b 0
:failed
popd
exit /b 1
