@echo off
setlocal
pushd "%~dp0..\..\.."
if errorlevel 1 exit /b 1
if defined VSCMD_ARG_TGT_ARCH goto tools_ready
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" goto missing_tools
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TASK_VSINSTALL=%%i"
if not defined TASK_VSINSTALL goto missing_tools
call "%TASK_VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 goto failed
:tools_ready
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" goto wrong_arch
if not exist "build\legacy-asi-tests" mkdir "build\legacy-asi-tests"
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DFIXTURE_ID=1 utils\legacy-asi\tests\fixture.cpp /Fobuild\legacy-asi-tests\fixture_one.obj /Febuild\legacy-asi-tests\fixture_one.asi /link /IMPLIB:build\legacy-asi-tests\fixture_one.lib
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DFIXTURE_ID=2 utils\legacy-asi\tests\fixture.cpp /Fobuild\legacy-asi-tests\fixture_two.obj /Febuild\legacy-asi-tests\fixture_two.asi /link /IMPLIB:build\legacy-asi-tests\fixture_two.lib
if errorlevel 1 goto failed
cl /nologo /c /utf-8 /std:c++20 /W4 /WX /MT /GS- /JMC- /DREVC_LEGACY_ASI /DREVC_LEGACY_ASI_CONSOLE_ENTRY src\extras\legacy_asi_entry.cpp /Fobuild\legacy-asi-tests\legacy_asi_entry.obj
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /DREVC_LEGACY_ASI /Isrc\extras utils\legacy-asi\tests\memory_tests.cpp src\extras\legacy_asi_memory.cpp build\legacy-asi-tests\legacy_asi_entry.obj /Fobuild\legacy-asi-tests\ /Febuild\legacy-asi-tests\memory_tests.exe /link /BASE:0x20000000 /DYNAMICBASE /FIXED:NO /SUBSYSTEM:CONSOLE /ENTRY:LegacyAsiEntry
if errorlevel 1 goto failed
if /i "%~1"=="--run" (
    build\legacy-asi-tests\memory_tests.exe
    if errorlevel 1 goto failed
) else (
    echo Built only. Pass --run to execute the local regression fixtures.
)
popd
exit /b 0

:missing_tools
echo Visual Studio C++ x86 build tools were not found.
goto failed
:wrong_arch
echo Use an x86 developer shell, not %VSCMD_ARG_TGT_ARCH%.
:failed
popd
exit /b 1
