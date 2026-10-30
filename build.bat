@echo off
setlocal enabledelayedexpansion

set "PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;%PATH%"
cd /d "%~dp0"

echo Building SwitchView for Windows (x86_64)...
mingw32-make.exe %*

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo.
echo Build successful: app\switchview.exe
