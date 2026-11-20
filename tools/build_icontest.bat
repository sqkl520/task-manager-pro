@echo off
cd /d "%~dp0.."
set "VSPATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [ERROR] BuildTools not found
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist build mkdir build
cl /nologo /utf-8 /EHsc /O2 /MD /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   tools\icontest.cpp /Fe:build\icontest.exe /Fo:build\ ^
   /link /SUBSYSTEM:CONSOLE ntdll.lib shell32.lib user32.lib ole32.lib advapi32.lib
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD OK
