@echo off
setlocal
rem Compile-only: never runs Wine, the game, or any newly built EXE/ASI.
pushd "%~dp0..\..\.."
if errorlevel 1 exit /b 1
if not "%~1"=="" goto unexpected_argument
if defined VSCMD_ARG_TGT_ARCH goto tools_ready
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" goto missing_tools
for /f "usebackq tokens=*" %%i in (`call "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TASK_WINE_PROBE_VS=%%i"
if not defined TASK_WINE_PROBE_VS goto missing_tools
call "%TASK_WINE_PROBE_VS%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 goto failed
:tools_ready
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" goto wrong_arch
set "TASK_WINE_PROBE_OUT=build\legacy-asi-wine-probe"
if not exist "%TASK_WINE_PROBE_OUT%\legacy-asi" mkdir "%TASK_WINE_PROBE_OUT%\legacy-asi"
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /DREVC_LAUNCHER_CONSOLE utils\legacy-asi\launcher\launcher.cpp utils\legacy-asi\launcher\debug_tls.cpp /Fo%TASK_WINE_PROBE_OUT%\ /Fe%TASK_WINE_PROBE_OUT%\reVC.exe /link /BASE:0x20000000 /DYNAMICBASE /FIXED:NO
if errorlevel 1 goto failed
cl /nologo /c /utf-8 /std:c++20 /W4 /WX /MT /GS- /JMC- /DREVC_LEGACY_ASI utils\legacy-asi\launcher\bootstrap_entry.cpp /Fo%TASK_WINE_PROBE_OUT%\bootstrap_entry.obj
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /DREVC_LEGACY_ASI utils\legacy-asi\launcher\bootstrap.cpp src\extras\legacy_asi_identity.cpp %TASK_WINE_PROBE_OUT%\bootstrap_entry.obj /Fo%TASK_WINE_PROBE_OUT%\ /Fe%TASK_WINE_PROBE_OUT%\reVC-game.exe /link /BASE:0x400000 /FIXED /DYNAMICBASE:NO /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /ENTRY:LegacyBootstrapEntry /IMPLIB:%TASK_WINE_PROBE_OUT%\bootstrap.lib
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DREVC_LEGACY_ASI /DREVC_LEGACY_ASI_CORE_DLL utils\legacy-asi\linux\canonical_probe.cpp src\extras\legacy_asi_memory.cpp src\extras\legacy_asi_entry.cpp src\extras\legacy_asi_loader.cpp src\extras\legacy_asi_identity.cpp /Fo%TASK_WINE_PROBE_OUT%\ /Fe%TASK_WINE_PROBE_OUT%\reVC-core.dll /link /BASE:0x20000000 /DYNAMICBASE /FIXED:NO /IMPLIB:%TASK_WINE_PROBE_OUT%\probe-core.lib
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DFIXTURE_ID=1 utils\legacy-asi\tests\fixture.cpp /Fo%TASK_WINE_PROBE_OUT%\fixture_one.obj /Fe%TASK_WINE_PROBE_OUT%\legacy-asi\fixture_one.asi /link /IMPLIB:%TASK_WINE_PROBE_OUT%\fixture-one.lib
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD /DFIXTURE_ID=2 utils\legacy-asi\tests\fixture.cpp /Fo%TASK_WINE_PROBE_OUT%\fixture_two.obj /Fe%TASK_WINE_PROBE_OUT%\legacy-asi\fixture_two.asi /link /IMPLIB:%TASK_WINE_PROBE_OUT%\fixture-two.lib
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /MT /LD utils\legacy-asi\launcher\identity_fixture.cpp /Fo%TASK_WINE_PROBE_OUT%\identity_fixture.obj /Fe%TASK_WINE_PROBE_OUT%\legacy-asi\identity_fixture.asi /link /IMPLIB:%TASK_WINE_PROBE_OUT%\fixture-identity.lib
if errorlevel 1 goto failed
> "%TASK_WINE_PROBE_OUT%\legacy-asi\enabled.ini" echo [Modules]
call :hash_module fixture_one.asi
if errorlevel 1 goto failed
call :hash_module fixture_two.asi
if errorlevel 1 goto failed
call :hash_module identity_fixture.asi
if errorlevel 1 goto failed
echo Built source-only canonical probe in %TASK_WINE_PROBE_OUT%.
echo Run its reVC.exe under Wine on Linux; require exit 0 and canonical-probe-result.txt status=PASS.
popd
exit /b 0

:hash_module
set "TASK_WINE_PROBE_HASH="
for /f "skip=1 delims=" %%h in ('certutil.exe -hashfile "%TASK_WINE_PROBE_OUT%\legacy-asi\%~1" SHA256') do if not defined TASK_WINE_PROBE_HASH set "TASK_WINE_PROBE_HASH=%%h"
if not defined TASK_WINE_PROBE_HASH exit /b 1
set "TASK_WINE_PROBE_HASH=%TASK_WINE_PROBE_HASH: =%"
if "%TASK_WINE_PROBE_HASH:~63,1%"=="" exit /b 1
if not "%TASK_WINE_PROBE_HASH:~64,1%"=="" exit /b 1
echo(%TASK_WINE_PROBE_HASH%| findstr.exe /r /i "^[0-9a-f]*$" >nul
if errorlevel 1 exit /b 1
>> "%TASK_WINE_PROBE_OUT%\legacy-asi\enabled.ini" echo %~1=%TASK_WINE_PROBE_HASH%
exit /b 0

:unexpected_argument
echo Compile only. This helper does not accept execution flags.
goto failed
:missing_tools
echo Visual Studio 2022 C++ x86 build tools were not found.
goto failed
:wrong_arch
echo Use an x86 developer shell, not %VSCMD_ARG_TGT_ARCH%.
:failed
popd
exit /b 1
