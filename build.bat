@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
    if "!VSPATH!"=="" (
        for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -all -products * -property installationPath 2^>nul`) do (
            if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%i"
        )
    )
)
if "!VSPATH!"=="" (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
)
if "!VSPATH!"=="" (
    echo [ERROR] No Visual Studio with C++ tools found.
    exit /b 1
)
call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [ERROR] vcvars64.bat failed. & exit /b 1 )

if not exist build mkdir build
set "CFLAGS=/nologo /utf-8 /std:c++17 /EHsc /O2 /W3 /MD /I include /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN"

echo.
echo [1/3] bench_snapshot.exe
cl %CFLAGS% src\bench_snapshot.cpp src\collector.cpp src\netproc.cpp src\gpuproc.cpp /Fe:build\bench_snapshot.exe /Fo:build\ /link /SUBSYSTEM:CONSOLE tdh.lib
if errorlevel 1 goto :fail

echo.
echo [2/3] bench_ui.exe
cl %CFLAGS% src\bench_ui.cpp src\tableview.cpp /Fe:build\bench_ui.exe /Fo:build\ /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

echo.
echo [3/3] TaskManagerPro.exe
cl %CFLAGS% src\main.cpp src\collector.cpp src\classifier.cpp ^
   src\tmwindow.cpp src\tmwindow_paint.cpp src\tmwindow_list.cpp src\tmwindow_events.cpp ^
   src\tmwindow_menus.cpp ^
   src\tmwindow_toolbar.cpp src\tmwindow_offscreen.cpp ^
   src\tmwindow_icons.cpp src\tmwindow_details.cpp src\usercache.cpp src\iconcache.cpp ^
   src\tabs.cpp src\menus.cpp src\drivepath.cpp src\perfdata.cpp src\perfpage.cpp ^
   src\gpuinfo.cpp ^
   src\pages_data_startup.cpp src\pages_data_users_services.cpp src\pages_view.cpp ^
   src\netproc.cpp src\gpuproc.cpp ^
   /Fe:build\TaskManagerPro.exe /Fo:build\ /link /SUBSYSTEM:WINDOWS iphlpapi.lib shell32.lib ole32.lib advapi32.lib pdh.lib powrprof.lib wtsapi32.lib version.lib shlwapi.lib tdh.lib
if errorlevel 1 goto :fail

echo.
echo ============================================
echo  BUILD SUCCEEDED
echo ============================================
exit /b 0

:fail
echo.
echo [ERROR] Build failed.
exit /b 1
