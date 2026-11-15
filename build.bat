@echo off
setlocal enabledelayedexpansion

:: Set project root
cd /d "%~dp0"

:: Environment variables
set "VS_BT=C:\VS-BT"
set "JAVA_HOME=C:\Users\User\scoop\apps\temurin17-jdk\current"
set "QT_DIR=C:\Qt\6.8.1\msvc2022_64"
set "VCPKG_ROOT=%VS_BT%\VC\vcpkg"
set "CMAKE_PREFIX_PATH=%QT_DIR%\lib\cmake"
set "BUILD_PLATFORM=official"
set "ARTIFACT_NAME=FreesmLauncher-Windows-Qt6"

echo ===================================================
echo  Building FreesmLauncher (Windows MSVC x64)
echo ===================================================
echo Project root:       %CD%
echo VS Build Tools:     %VS_BT%
echo Java Home:          %JAVA_HOME%
echo Qt Directory:       %QT_DIR%
echo Vcpkg Root:         %VCPKG_ROOT%
echo ===================================================

:: Initialize Visual Studio MSVC 64-bit environment
if not defined DevEnvDir (
    if exist "%VS_BT%\VC\Auxiliary\Build\vcvars64.bat" (
        echo Initializing MSVC x64 environment...
        call "%VS_BT%\VC\Auxiliary\Build\vcvars64.bat"
        if errorlevel 1 (
            echo [ERROR] Failed to initialize vcvars64.bat
            exit /b 1
        )
    ) else (
        echo [ERROR] Could not find vcvars64.bat at %VS_BT%\VC\Auxiliary\Build\vcvars64.bat
        exit /b 1
    )
)

:: Ensure Java and Qt are on PATH for compilation and deployment
set "PATH=%JAVA_HOME%\bin;%QT_DIR%\bin;%PATH%"

:: 1. Configure
echo.
echo [1/3] Configuring project with CMake...
cmake --preset windows_msvc
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b %errorlevel%
)

:: 2. Build
echo.
echo [2/3] Building FreesmLauncher (Release)...
cmake --build build --config Release
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b %errorlevel%
)

:: 3. Install / Stage complete bundle
echo.
echo [3/3] Staging installation and dependencies to ./install...
cmake --install build --config Release
if errorlevel 1 (
    echo [ERROR] Installation failed.
    exit /b %errorlevel%
)

:: Also install portable files
cmake --install build --config Release --component portable

echo.
echo ===================================================
echo  Build and packaging completed successfully!
echo  Executable and runtime files: %CD%\install
echo ===================================================
exit /b 0
