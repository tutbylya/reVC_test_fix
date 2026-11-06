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
set "TASK_LEGACY_TEST_OUT=build\legacy-asi-tests"
if defined REVC_LEGACY_TEST_OUTPUT set "TASK_LEGACY_TEST_OUT=%REVC_LEGACY_TEST_OUTPUT%"
if not exist "%TASK_LEGACY_TEST_OUT%" mkdir "%TASK_LEGACY_TEST_OUT%"
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DFIXTURE_ID=1 utils\legacy-asi\tests\fixture.cpp /Fo"%TASK_LEGACY_TEST_OUT%\fixture_one.obj" /Fe"%TASK_LEGACY_TEST_OUT%\fixture_one.asi" /link /IMPLIB:"%TASK_LEGACY_TEST_OUT%\fixture_one.lib"
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DFIXTURE_ID=2 utils\legacy-asi\tests\fixture.cpp /Fo"%TASK_LEGACY_TEST_OUT%\fixture_two.obj" /Fe"%TASK_LEGACY_TEST_OUT%\fixture_two.asi" /link /IMPLIB:"%TASK_LEGACY_TEST_OUT%\fixture_two.lib"
if errorlevel 1 goto failed
rem The image owns .compat before Windows allocates heaps/stacks. This is the
rem production canonical-image model, not a retry of a failed private arena.
rem CORE_DLL suppresses the obsolete private pre-CRT entry; use normal main CRT.
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /DREVC_LEGACY_ASI /DREVC_LEGACY_ASI_CORE_DLL /Isrc\extras utils\legacy-asi\tests\memory_tests.cpp utils\legacy-asi\tests\canonical_test_arena.cpp src\extras\legacy_asi_memory.cpp src\extras\legacy_asi_entry.cpp /Fo"%TASK_LEGACY_TEST_OUT%\\" /Fe"%TASK_LEGACY_TEST_OUT%\memory_tests.exe" /link /BASE:0x400000 /FIXED /DYNAMICBASE:NO /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /IMPLIB:"%TASK_LEGACY_TEST_OUT%\memory_tests.lib"
if errorlevel 1 goto failed
if /i "%~1"=="--run" (
    "%TASK_LEGACY_TEST_OUT%\memory_tests.exe"
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
