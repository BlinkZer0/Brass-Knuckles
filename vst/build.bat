@echo off
rem ==========================================================================
rem  Builds the Brass Knuckles VST3 + Standalone (Release) and the test tool.
rem  Usage:  build.bat [Debug|Release]
rem  Build output and the JUCE download go to %LOCALAPPDATA%\BrassKnucklesVST
rem  so OneDrive doesn't try to sync ~1 GB of intermediate files.
rem  If E-Piano already downloaded JUCE 8.0.17, that copy is reused.
rem ==========================================================================
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "WORK=%LOCALAPPDATA%\BrassKnucklesVST"
set "BUILD=%WORK%\build"

rem --- find CMake (PATH first, then the copy bundled with Visual Studio) ---
set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

rem --- git is needed to fetch JUCE ---
where git >nul 2>nul
if errorlevel 1 if exist "%ProgramFiles%\Git\cmd\git.exe" set "PATH=%ProgramFiles%\Git\cmd;%PATH%"

rem --- reuse an existing JUCE checkout (E-Piano's) instead of downloading again ---
set "JUCE_ARG="
if not exist "%WORK%\deps\juce-src\CMakeLists.txt" if exist "%LOCALAPPDATA%\EPianoVST\deps\juce-src\CMakeLists.txt" set "JUCE_ARG=-DBK_JUCE_DIR=%LOCALAPPDATA%\EPianoVST\deps\juce-src"

echo [Brass Knuckles] configuring...
"%CMAKE%" -S "%~dp0." -B "%BUILD%" -DFETCHCONTENT_BASE_DIR="%WORK%\deps" %JUCE_ARG% || goto :fail

echo [Brass Knuckles] building %CONFIG%...
"%CMAKE%" --build "%BUILD%" --config %CONFIG% --target BrassKnucklesVST_VST3 BrassKnucklesVST_Standalone BrassKnucklesTests --parallel || goto :fail

echo.
echo [Brass Knuckles] Done.
echo   VST3       : %BUILD%\BrassKnucklesVST_artefacts\%CONFIG%\VST3\Brass Knuckles.vst3
echo   Standalone : %BUILD%\BrassKnucklesVST_artefacts\%CONFIG%\Standalone\Brass Knuckles.exe
echo   Tests      : %BUILD%\BrassKnucklesTests_artefacts\%CONFIG%\BrassKnucklesTests.exe
echo   Run install.bat to copy the VST3 into C:\Program Files\Common Files\VST3
exit /b 0

:fail
echo [Brass Knuckles] BUILD FAILED
exit /b 1
