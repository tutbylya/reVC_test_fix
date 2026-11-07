@echo off
setlocal
pushd "%~dp0..\.."
if errorlevel 1 exit /b 1
if defined VSCMD_ARG_TGT_ARCH goto tools_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TASK_VSINSTALL=%%i"
if not defined TASK_VSINSTALL goto failed
call "%TASK_VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto failed
:tools_ready
if not exist "build\resource-parser-tests" mkdir "build\resource-parser-tests"
cl /nologo /utf-8 /std:c++17 /EHsc /W4 /WX /MT utils\modloader\test-resource-parsers.cpp /Fobuild\resource-parser-tests\ /Febuild\resource-parser-tests\resource-parser-tests.exe
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++17 /EHsc /W4 /WX /MT /Isrc\extras utils\tests\remastered_ui_tests.cpp src\extras\remastered_ui.cpp /Fobuild\resource-parser-tests\ /Febuild\resource-parser-tests\native-ui-tests.exe
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++17 /EHsc /W4 /WX /MT utils\modloader\test-mod-menu-state.cpp src\extras\modloader_folder.cpp /Fobuild\resource-parser-tests\ /Febuild\resource-parser-tests\mod-menu-state-tests.exe
if errorlevel 1 goto failed
cl /nologo /utf-8 /std:c++17 /EHsc /W4 /WX /MT utils\modloader\test-mod-menu-text.cpp /Fobuild\resource-parser-tests\ /Febuild\resource-parser-tests\mod-menu-text-tests.exe
if errorlevel 1 goto failed
if /i "%~1"=="--build-only" goto built
build\resource-parser-tests\mod-menu-state-tests.exe
if errorlevel 1 goto failed
build\resource-parser-tests\mod-menu-text-tests.exe
if errorlevel 1 goto failed
build\resource-parser-tests\native-ui-tests.exe
if errorlevel 1 goto failed
if exist "build\modloader-game-test\data\handling.cfg" (
    build\resource-parser-tests\resource-parser-tests.exe build\modloader-game-test\data\handling.cfg
) else (
    build\resource-parser-tests\resource-parser-tests.exe
)
if errorlevel 1 goto failed
:built
popd
exit /b 0
:failed
popd
exit /b 1
