@echo off
rem Builds the engine and the app for a PC, for testing, as build.cmd does, but in the same folder again only compiles what changed since:
rem the files changed, and every file that includes a header that changed (host.ninja). The test programs' own code isn't optimized, so a
rem change to a test builds in seconds.
rem   build_fast.cmd <output folder>
if "%~1"=="" (
    echo build_fast.cmd ^<output folder^>
    exit /b 2
)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
rem Ninja learns which headers a file includes from what the compiler says as it compiles it, which it only reads in English.
set VSLANG=1033
set OUT=%~f1
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
rem In what Ninja reads as a path, a colon is written $:, as one on its own ends the path.
set HERE=%~dp0
set HERE=%HERE::=$:%
(
    echo sw = %~dp0..
    echo include %HERE%host.ninja
) > "%OUT%\build.ninja"
"%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C "%OUT%" -j 4
exit /b %ERRORLEVEL%
