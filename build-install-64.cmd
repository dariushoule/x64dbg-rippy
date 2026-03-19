@echo off

:: Find and invoke VS2022 developer environment
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: Visual Studio not found
    exit /b 1
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -no_logo

set INSTALL_DIR=C:\re\x64dbg_dev\release\x64\plugins\

if not exist %INSTALL_DIR% mkdir %INSTALL_DIR%

:: Build frontend UI
pushd "%~dp0frontend"
call npm install
call npm run build
popd

:: Build C++ plugin
cd /d "%~dp0"
cmake -B build64 -G "Visual Studio 17 2022" -A x64
cmake --build build64 --config Release
copy /Y build64\Release\x64dbg-rippy.dp64 %INSTALL_DIR%\
copy /Y build64\Release\libcrypto-3-x64.dll %INSTALL_DIR%\
copy /Y build64\Release\libssl-3-x64.dll %INSTALL_DIR%\
copy /Y build64\Release\brotlicommon.dll %INSTALL_DIR%\
copy /Y build64\Release\brotlidec.dll %INSTALL_DIR%\
copy /Y build64\Release\WebView2Loader.dll %INSTALL_DIR%\
