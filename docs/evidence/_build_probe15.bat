@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /W3 /MD /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN gpuinfo_probe15.cpp /Fe:gpuinfo_probe15.exe /Fo:gpuinfo_probe15.obj /link /SUBSYSTEM:CONSOLE dxgi.lib advapi32.lib user32.lib ole32.lib gdi32.lib
