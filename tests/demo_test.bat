@echo off
rem Example of driving a test from a .bat file. Each step is a separate process
rem talking to one long-running "serve" process, so periodic traffic never stops
rem between steps and the script can branch on errorlevel.
rem For real hardware, change "open sim" to "open ftdi".
setlocal
set CLI=%~dp0..\bin\Release\IcdLinkCli.exe

start "" /b "%CLI%" serve
"%CLI%" call open sim || goto :fail
"%CLI%" call cyclic C on || goto :fail

rem Set several values as one update, wait for the answer, then decide.
"%CLI%" call begin then set ToC.FakeIcd.Power 1 then set ToC.FakeIcd.Test1 7 then sync || goto :fail
"%CLI%" call expect FromC.FakeIcd.Test1 == 7
if errorlevel 1 goto :abort
"%CLI%" call set ToC.FakeIcd.Test2 3 then end || goto :fail

"%CLI%" call sync || goto :fail
"%CLI%" call expect FromC.FakeIcd.Test2 == 3 within 200 || goto :fail
"%CLI%" call dump FromC.FakeIcd
"%CLI%" call stats C
"%CLI%" call shutdown
echo TEST PASSED
exit /b 0

:abort
"%CLI%" call abort
:fail
set CODE=%errorlevel%
"%CLI%" call shutdown
echo TEST FAILED (%CODE%)
exit /b 1
