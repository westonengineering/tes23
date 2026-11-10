@echo off
rem build.bat [Debug^|Release] - regenerates from the ICD and builds everything with MSVC.
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release
cd /d "%~dp0"

python tools\icdgen.py icd\project.json || goto :fail

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if not defined MSBUILD (echo MSBuild was not found & goto :fail)

"%MSBUILD%" IcdLink.sln -m -restore -v:minimal -p:Configuration=%CONFIG% -p:Platform=x64 || goto :fail

echo.
echo Build succeeded: bin\%CONFIG%
exit /b 0

:fail
echo Build FAILED
exit /b 1
