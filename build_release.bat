@echo off
setlocal

set "ROOT=%~dp0"
set "SOLUTION=%ROOT%CodeRouteBridge.sln"
set "OUTPUT_DIR=%ROOT%build\Release"
set "OUTPUT_CONFIG=%OUTPUT_DIR%\CodeRouteBridge.ini"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
        if not defined MSBUILD set "MSBUILD=%%I"
    )
)

if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

if not defined MSBUILD (
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
        echo VsCodePath=%%LOCALAPPDATA%%\Programs\Microsoft VS Code\Code.exe
        echo VsCodeFileNameRegex=.*\.json$
        echo NotepadPlusPlusPath=%%ProgramFiles%%\Notepad++\notepad++.exe
        echo NotepadPlusPlusFileNameRegex=.*\.log$
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
