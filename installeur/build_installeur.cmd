@echo off
rem Construit l'installeur (programme Windows sans console) avec bin\d3d9.dll incluse.
rem Lancer d'abord ..\build.cmd. Meme recherche de Visual Studio (variable VCVARS32 possible).
setlocal
if not defined VCVARS32 (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2^>nul`) do set "VCVARS32=%%i\VC\Auxiliary\Build\vcvars32.bat"
)
if not defined VCVARS32 ( echo Visual Studio introuvable : definir VCVARS32. & exit /b 1 )
call "%VCVARS32%" >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\bin\d3d9.dll ( echo Lancer d'abord ..\build.cmd & exit /b 1 )
if not exist sortie mkdir sortie
powershell -NoProfile -Command "$t=[IO.File]::ReadAllText('JSR60_LISEZMOI.txt',[Text.Encoding]::UTF8); $t=$t -replace \"`r?`n\",\"`r`n\"; [IO.File]::WriteAllText('lisezmoi_utf8bom.txt',$t,(New-Object Text.UTF8Encoding $true))" || exit /b 2
rc /nologo /fo sortie\installeur.res installeur.rc || exit /b 3
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /utf-8 installeur.cpp sortie\installeur.res /Fo:sortie\ /Fe:"sortie\Installer JSR 60 FPS.exe" /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib ole32.lib advapi32.lib || exit /b 4
exit /b 0
