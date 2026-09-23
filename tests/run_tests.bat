@echo off
rem Builds and runs the out-of-game test (the real Store.cpp + Hooks.cpp, without CommonLib).
rem Needs the plugin build already configured (cmake --preset release) to find vcpkg's MinHook.
setlocal
set "ROOT=%~dp0.."
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "INST=%ROOT%\build\release\vcpkg_installed\x64-windows-static"
set "OUT=%ROOT%\build\tests"
if not exist "%OUT%" mkdir "%OUT%"
for %%L in ("%INST%\lib\minhook*.lib") do set "MHLIB=%%~fL"

cl /nologo /std:c++latest /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /FI"%ROOT%\tests\TestPCH.h" /I"%ROOT%\include" /I"%ROOT%\src" /I"%INST%\include" ^
   "%ROOT%\tests\test.cpp" "%ROOT%\src\Store.cpp" "%ROOT%\src\Hooks.cpp" "%MHLIB%" ^
   /Fo"%OUT%\\" /Fe"%OUT%\tff_test.exe" || exit /b 1

"%OUT%\tff_test.exe"
