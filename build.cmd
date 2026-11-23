@echo off
rem Builds rdpman.exe as a single native x64 executable (static CRT, no runtime dependencies).
setlocal
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio C++ build tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O1 /MT /W3 /DUNICODE /D_UNICODE rdpman.cpp /Fe:rdpman.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:app.manifest /OPT:REF /OPT:ICF || exit /b 1
del rdpman.obj 2>nul
echo Built %~dp0rdpman.exe
