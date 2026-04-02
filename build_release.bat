@echo off
setlocal

set "ROOT=%~dp0"
set "SOLUTION=%ROOT%CodeRouteBridge.sln"
set "OUTPUT_DIR=%ROOT%build\Release"
set "OUTPUT_CONFIG=%OUTPUT_DIR%\CodeRouteBridge.ini"
set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"

if not exist "%MSBUILD%" set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"

if not exist "%MSBUILD%" (
    echo MSBuild not found.
    exit /b 1
)

echo Building Release x64...
"%MSBUILD%" "%SOLUTION%" /t:Rebuild /p:Configuration=Release /p:Platform=x64
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"
if not exist "%OUTPUT_CONFIG%" (
    >"%OUTPUT_CONFIG%" (
        echo [General]
        echo VsCodePath=C:\Users\4456\AppData\Local\Programs\Microsoft VS Code\Code.exe
        echo ForceOpenInVsCode=0
        echo ShowMainWindow=0
        echo WaitForDocumentMs=3000
        echo PollIntervalMs=100
    )
)

echo.
echo Build completed.
echo Release executable: %OUTPUT_DIR%\Code.exe
echo Release config: %OUTPUT_CONFIG%
exit /b 0
