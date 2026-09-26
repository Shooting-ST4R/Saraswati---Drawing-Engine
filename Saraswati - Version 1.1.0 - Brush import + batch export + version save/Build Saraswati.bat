@echo off
rem Builds Saraswati.exe with Visual Studio (MSVC) + CMake + Ninja and copies it to App-Data\App\.
setlocal
cd /d "%~dp0App-Data\Source"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio was not found. Install Visual Studio 2022 or its Build Tools with
  echo the "Desktop development with C++" workload ^(includes CMake and Ninja^).
  goto :fail
)
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo No Visual Studio installation with the C++ x64 tools was found.
  goto :fail
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :fail

where git >nul 2>nul || (echo git is required to fetch the libraries. & goto :fail)
where cmake >nul 2>nul || (echo cmake not found - add the "C++ CMake tools for Windows" VS component. & goto :fail)
where ninja >nul 2>nul || (echo ninja not found - add the "C++ CMake tools for Windows" VS component. & goto :fail)

echo === Fetching libraries (first run only) ===
powershell -NoProfile -ExecutionPolicy Bypass -File fetch-deps.ps1
if errorlevel 1 goto :fail

echo === Configuring ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fail

echo === Building ===
cmake --build build
if errorlevel 1 goto :fail

if not exist "..\App" mkdir "..\App"
copy /y "build\Saraswati.exe" "..\App\Saraswati.exe" >nul
if errorlevel 1 goto :fail
echo.
echo Done: App-Data\App\Saraswati.exe
pause
exit /b 0

:fail
echo.
echo BUILD FAILED
pause
exit /b 1
