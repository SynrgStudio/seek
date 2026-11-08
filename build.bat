@echo off
setlocal
rem Builds Seek (build\seek.exe) with Visual Studio's C++ compiler. Stops its background process first, so the
rem executable can be replaced.
cd /d "%~dp0"
if not exist build mkdir build
if exist build\seek.exe build\seek.exe --stop

rem the compiler's environment, unless this console already has it
where cl >nul 2>nul && goto env_ok
set VSDIR=
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with C++ not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:env_ok

rc /nologo /fo build\seek.res src\seek.rc || exit /b 1
cl /nologo /std:c++20 /O2 /W4 /EHsc /utf-8 /permissive- /MT /Zi /DUNICODE /D_UNICODE /DNOMINMAX /Fobuild\ /Fdbuild\ /Febuild\seek.exe src\seek.cpp build\seek.res ^
  /link /DEBUG /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE || exit /b 1
echo Done: build\seek.exe
