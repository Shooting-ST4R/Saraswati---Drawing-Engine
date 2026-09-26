@echo off
rem Starts Saraswati on SwiftShader (CPU Vulkan from the Chrome / Edge install) - for machines
rem without a Vulkan GPU. Correctness only: slow, and limited to 8192 px documents.
if not exist "%~dp0App-Data\App\Saraswati.exe" (
  echo Saraswati.exe not found - run "Build Saraswati.bat" first.
  pause
  exit /b 1
)
start "" "%~dp0App-Data\App\Saraswati.exe" --cpu-vulkan %*
