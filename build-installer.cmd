@echo off
REM ===========================================================================
REM Builds the Release binaries and packages them into DirSizeForExplorer.msi
REM
REM Prerequisites (one-time):
REM   - Visual Studio 2022 + CMake, VCPKG_ROOT set (see README)
REM   - WiX v4+ CLI:  dotnet tool install --global wix
REM     (extensions are restored automatically from the .wix folder;
REM      otherwise: wix extension add WixToolset.Util.wixext
REM                 wix extension add WixToolset.UI.wixext)
REM ===========================================================================
setlocal
cd /d "%~dp0"

REM --- Locate Visual Studio (for bundled cmake/vcpkg fallbacks) ---
REM NOTE: no parenthesized blocks around these lookups - %ProgramFiles(x86)%
REM expands a ")" that cmd's block parser trips over inside "( ... )".
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" call :locatevs

REM --- Locate cmake: PATH first, then Visual Studio's bundled copy ---
set CMAKE=cmake
where cmake >nul 2>nul
if not errorlevel 1 goto :havecmake
if defined VSDIR set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%CMAKE%" goto :havecmake
echo ERROR: cmake not found on PATH and no Visual Studio installation found.
echo Run this script from a Visual Studio "Developer Command Prompt" instead.
exit /b 1
:havecmake
echo Using cmake: %CMAKE%

REM --- Locate vcpkg: VCPKG_ROOT if set, else Visual Studio's bundled copy
REM     (the CMake preset resolves the toolchain from VCPKG_ROOT) ---
if defined VCPKG_ROOT goto :havevcpkg
if defined VSDIR call :trysetvcpkg "%VSDIR%\VC\vcpkg"
if defined VCPKG_ROOT goto :havevcpkg
echo ERROR: VCPKG_ROOT is not set and no bundled vcpkg was found.
echo Install vcpkg and set VCPKG_ROOT, or install the vcpkg component in Visual Studio.
exit /b 1
:havevcpkg
echo Using vcpkg: %VCPKG_ROOT%

echo === 1/3 Building Release binaries ===
call "%CMAKE%" --preset default || goto :fail
call "%CMAKE%" --build build --config Release || goto :fail

echo === 2/3 Staging files ===
set STAGE=build\installer-stage
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
REM sqlite3 and the CRT are linked statically (x64-windows-static triplet
REM + /MT) - no runtime DLLs to stage, and no VC++ redistributable needed.
copy /y "build\shell-ext\Release\DirSizeShellExt.dll"      "%STAGE%\" >nul || goto :fail
copy /y "build\shell-ext\Release\DirSizeTotalSize.propdesc" "%STAGE%\" >nul || goto :fail
copy /y "build\tray\Release\DirSizeTray.exe"               "%STAGE%\" >nul || goto :fail

echo === 3/3 Building MSI ===
wix build installer\Product.wxs installer\Components.wxs ^
    -d BuildDir=%STAGE% ^
    -o build\DirSizeForExplorer.msi ^
    -arch x64 ^
    -ext WixToolset.Util.wixext ^
    -ext WixToolset.UI.wixext || goto :fail

echo.
echo SUCCESS: build\DirSizeForExplorer.msi
exit /b 0

:fail
echo.
echo BUILD FAILED (see output above)
exit /b 1

:locatevs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
exit /b 0

:trysetvcpkg
if exist "%~1\scripts\buildsystems\vcpkg.cmake" set "VCPKG_ROOT=%~1"
exit /b 0
