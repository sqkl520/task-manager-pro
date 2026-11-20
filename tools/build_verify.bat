@echo off
cd /d "%~dp0.."
set "VSPATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist build mkdir build
cl /nologo /utf-8 /EHsc /O2 /MD /DUNICODE /D_UNICODE /DNOMINMAX /I include ^
   tools\verify_fix.cpp /Fe:build\verify_fix.exe /Fo:build\ ^
   /link /SUBSYSTEM:CONSOLE ntdll.lib shell32.lib user32.lib ole32.lib
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo BUILD OK
