@echo off
rem Starts Saraswati on the default (fastest) GPU. Extra arguments are passed through.
if not exist "%~dp0App-Data\App\Saraswati.exe" (
  echo Saraswati.exe not found - run "Build Saraswati.bat" first.
  pause
  exit /b 1
)
start "" "%~dp0App-Data\App\Saraswati.exe" %*
