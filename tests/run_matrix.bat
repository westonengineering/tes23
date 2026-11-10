@echo off
rem run_matrix.bat - tests channels C and D on the simulator, periodic and manual,
rem with and without half-duplex echo, on the normal ICD and on a fake 375-byte ICD.
rem Run build.bat first.
setlocal
cd /d "%~dp0.."
set ROOT=%CD%

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
set COMMON=-m -v:quiet -nologo -p:Configuration=Release -p:Platform=x64

echo === Generating fake large ICD
python tests\large\make_fake_icd.py tests\large\fake_large_icd.txt || exit /b 1
python tools\icdgen.py tests\large\project_large.json --out tests\large\generated > tests\large\generator_report.txt || exit /b 1

echo === Building
"%MSBUILD%" native\IcdMatrixTests.vcxproj %COMMON% || exit /b 1
set LARGE="-p:IcdGeneratedDir=%ROOT%\tests\large\generated" "-p:OutDir=%ROOT%\bin\large\\"
"%MSBUILD%" native\IcdMatrixTests.vcxproj %COMMON% %LARGE% "-p:IntDir=%ROOT%\obj\large\IcdMatrixTests\\" || exit /b 1
"%MSBUILD%" native\IcdLink.vcxproj %COMMON% %LARGE% "-p:IntDir=%ROOT%\obj\large\IcdLink\\" || exit /b 1

set FAILED=0
echo.
echo === Normal ICD (Fake ICD on C, demo on D)
bin\Release\IcdMatrixTests.exe bin\Release\matrix_dump.txt || set FAILED=1
python tests\check_wire.py bin\Release\matrix_dump.txt || set FAILED=1

echo.
echo === Fake large ICD (375 bytes sent, 275 returned)
bin\large\IcdMatrixTests.exe bin\large\matrix_dump.txt || set FAILED=1
python tests\check_wire.py bin\large\matrix_dump.txt || set FAILED=1

echo.
if %FAILED%==0 (echo ALL PASSED) else (echo FAILED)
exit /b %FAILED%
