@echo off
rem Compile la d3d9.dll (32 bits) de Jet Set Radio 60 FPS -> bin\d3d9.dll
rem Necessite Visual Studio 2022 (ou Build Tools) avec les outils C++ x86.
rem Si Visual Studio n'est pas trouve automatiquement, definir VCVARS32 = chemin complet de vcvars32.bat.
setlocal
if not defined VCVARS32 (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2^>nul`) do set "VCVARS32=%%i\VC\Auxiliary\Build\vcvars32.bat"
)
if not defined VCVARS32 ( echo Visual Studio introuvable : definir VCVARS32. & exit /b 1 )
call "%VCVARS32%" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /O2 /Oy- /D_CRT_SECURE_NO_WARNINGS /MT /EHsc /std:c++17 /W3 /LD src\d3d9_proxy.cpp /Fo:bin\ /Fe:bin\d3d9.dll /link /DEF:src\d3d9.def user32.lib || exit /b 2
exit /b 0
