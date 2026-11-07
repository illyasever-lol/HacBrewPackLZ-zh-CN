@echo off
rem Builds the engine and the app for a PC, for testing, with Visual Studio's compiler.
rem   build.cmd <output folder>
if "%~1"=="" (
    echo build.cmd ^<output folder^>
    exit /b 2
)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set SW=%~dp0..
set OUT=%~f1
set FLAGS=/nologo /std:c++17 /O2 /EHsc /W3 /utf-8 /wd4267 /wd4244 /wd4996 /D_CRT_SECURE_NO_WARNINGS /D_SILENCE_CXX17_U8PATH_DEPRECATION_WARNING /DNOMINMAX /DWIN32_LEAN_AND_MEAN
set CORE=%SW%\core\common.cpp %SW%\core\crypto.cpp %SW%\core\source.cpp %SW%\core\lz4.cpp %SW%\core\zstd.cpp %SW%\core\ncz.cpp %SW%\core\nca.cpp %SW%\core\nacp.cpp %SW%\core\ncaout.cpp %SW%\core\title.cpp %SW%\core\build.cpp %SW%\core\console.cpp %SW%\core\remote.cpp %SW%\host\host_sys.cpp %SW%\host\host_remote.cpp
set APP=%SW%\app\canvas.cpp %SW%\app\app.cpp %SW%\app\app_saved.cpp %SW%\app\app_lists.cpp %SW%\app\app_work.cpp %SW%\app\app_actions.cpp %SW%\app\app_input.cpp %SW%\app\app_draw.cpp
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl %FLAGS% /c /Fo:"%OUT%\obj\\" %CORE% %APP% %SW%\host\sim_console.cpp %SW%\host\hbptool.cpp %SW%\host\replace_test.cpp %SW%\host\app_test.cpp /MP4
if errorlevel 1 exit /b 1
cd /d "%OUT%\obj"
set COREOBJ=common.obj crypto.obj source.obj lz4.obj zstd.obj ncz.obj nca.obj nacp.obj ncaout.obj title.obj build.obj console.obj remote.obj host_sys.obj host_remote.obj
link /nologo /OUT:"%OUT%\hbptool.exe" %COREOBJ% hbptool.obj
if errorlevel 1 exit /b 1
link /nologo /OUT:"%OUT%\replace_test.exe" %COREOBJ% sim_console.obj replace_test.obj
if errorlevel 1 exit /b 1
link /nologo /OUT:"%OUT%\app_test.exe" %COREOBJ% sim_console.obj canvas.obj app.obj app_saved.obj app_lists.obj app_work.obj app_actions.obj app_input.obj app_draw.obj app_test.obj ole32.lib oleaut32.lib windowscodecs.lib
exit /b %ERRORLEVEL%
