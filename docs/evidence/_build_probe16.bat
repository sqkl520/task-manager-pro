@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /W3 /MD /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN gpuinfo_probe16.cpp /Fe:gpuinfo_probe16.exe /Fo:gpuinfo_probe16.obj /link /SUBSYSTEM:CONSOLE pdh.lib
