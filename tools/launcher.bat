@echo off
rem Builds (the first time, or after an update of the repository) and opens the launcher: the program that sets the
rem game up from your files and starts it (README, "Installation"). Windows; tools/launcher.sh on Linux and macOS.
rem   usage: tools\launcher.bat
rem Needs Python 3 and the Visual Studio C++ build tools (README, Requirements). tools\setup_acnh.py --launcher
rem downloads LLVM (clang-cl) and SDL3 into private\third_party the first time and builds build\launcher.
setlocal
cd /d "%~dp0.."
rem Python: the py launcher, else python on PATH (the Microsoft Store's stand-in, there until Python is installed,
rem fails --version).
set "PY="
py -3 --version >nul 2>nul
if not errorlevel 1 set "PY=py -3"
if not defined PY python --version >nul 2>nul
if not defined PY if not errorlevel 1 set "PY=python"
if not defined PY goto nopython

%PY% tools\setup_acnh.py --launcher
if errorlevel 1 goto failed
start "" "%CD%\build\launcher\acnhrecomp_launcher.exe" %*
exit /b 0

:nopython
echo Python 3 was not found. Install it with:
echo   winget install Python.Python.3.13
echo then open this again.
pause
exit /b 1

:failed
echo.
echo The launcher could not be built: the lines above say why, the full logs are in private\acnh\logs.
pause
exit /b 1
