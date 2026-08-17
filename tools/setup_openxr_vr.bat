@echo off
setlocal

REM Run this from the root of your iw4x-client clone.
if not exist "premake5.lua" (
    echo ERROR: Run this file from the iw4x-client repository root.
    exit /b 1
)

where git >nul 2>nul || (
    echo ERROR: Git is not available in PATH.
    exit /b 1
)
where cmake >nul 2>nul || (
    echo ERROR: CMake is not available in PATH.
    exit /b 1
)

if not exist "deps\openxr-sdk\.git" (
    echo Cloning Khronos OpenXR-SDK...
    git clone --depth 1 https://github.com/KhronosGroup/OpenXR-SDK.git deps\openxr-sdk || exit /b 1
)

set "XRBUILD=deps\openxr-sdk\build\iw4x-win32"

echo Configuring a 32-bit dynamic OpenXR loader...
cmake -S deps\openxr-sdk -B "%XRBUILD%" -G "Visual Studio 17 2022" -A Win32 -DDYNAMIC_LOADER=ON || exit /b 1

echo Building openxr_loader Release...
cmake --build "%XRBUILD%" --config Release --target openxr_loader || exit /b 1

if not exist "lib\include\openxr" mkdir "lib\include\openxr"
xcopy /Y /I /E "deps\openxr-sdk\include\openxr\*" "lib\include\openxr\" >nul || exit /b 1

if not exist "lib\openxr\win32" mkdir "lib\openxr\win32"
for /r "%XRBUILD%" %%F in (openxr_loader.lib) do copy /Y "%%F" "lib\openxr\win32\openxr_loader.lib" >nul
for /r "%XRBUILD%" %%F in (openxr_loader.dll) do copy /Y "%%F" "lib\openxr\win32\openxr_loader.dll" >nul

if not exist "lib\openxr\win32\openxr_loader.lib" (
    echo ERROR: openxr_loader.lib was not found after the build.
    exit /b 1
)
if not exist "lib\openxr\win32\openxr_loader.dll" (
    echo ERROR: openxr_loader.dll was not found after the build.
    exit /b 1
)

echo.
echo OpenXR loader is ready.
echo Headers: lib\include\openxr
echo Library: lib\openxr\win32\openxr_loader.lib
echo Runtime DLL: lib\openxr\win32\openxr_loader.dll
echo.
echo Next: run generate.bat, then build build\iw4x.sln as Win32 Release.
exit /b 0
