@echo off
rem Run Animal Crossing: New Horizons on Windows with a preset (tools/acnh_linux.sh on Linux).
rem   usage: tools\acnh_windows.bat [preset] [data folder]
rem   quality      docked 1080p, plain scaling to the window                                          [default]
rem   balanced     the game renders 720p (handheld mode) and FSR 1 upscales to the window
rem   intel        integrated GPU (Intel Iris Xe / UHD, AMD APUs): handheld 720p, pipelined submits, audio reverb off
rem   lightest     intel plus FSR 1
rem The data folder is the one tools\setup_acnh.py built: private\acnh, or its --out folder. The game is
rem <data folder>\build\acnh.exe. The audio reverb (I3DL2, rendered on the CPU) costs about one core;
rem NX_AUDIO_REVERB=0 drops it in any preset. Extra environment still applies (NX_FULLSCREEN=1, NX_RESOLUTION=WxH,
rem NX_MUTE=1, NX_MEM_LIMIT_GB, NX_LOG, NX_HEADLESS=1).
setlocal
cd /d "%~dp0.."
set "MODE=%~1"
if "%MODE%"=="" set "MODE=quality"
set "DATA=%~2"
if "%DATA%"=="" set "DATA=%CD%\private\acnh"

if /i "%MODE%"=="quality" goto run
if /i "%MODE%"=="balanced" goto balanced
if /i "%MODE%"=="intel" goto intel
if /i "%MODE%"=="lightest" goto lightest
echo unknown preset: %MODE% - one of quality, balanced, intel, lightest
exit /b 1

:lightest
set "NX_FSR=1"
:intel
if not defined NX_GPU set "NX_GPU=integrated"
if not defined NX_PIPELINED set "NX_PIPELINED=2"
if not defined NX_AUDIO_REVERB set "NX_AUDIO_REVERB=0"
set "NX_HANDHELD=1"
goto run
:balanced
set "NX_HANDHELD=1"
set "NX_FSR=1"

:run
if not exist "%DATA%\build\acnh.exe" goto notbuilt
set "NX_DATA_ROOT=%DATA%"
if not defined NX_LOG set "NX_LOG=%DATA%\nx_play.log"
"%DATA%\build\acnh.exe"
if errorlevel 1 goto crashed
exit /b 0

:notbuilt
echo The game is not built yet in %DATA%: run the setup first (README, Installation), for example
echo   py tools\setup_acnh.py --game C:\ACNH-game
pause
exit /b 1

:crashed
echo.
echo The game closed with an error. The log is %NX_LOG%; Settings ^> About ^> Create problem report makes a
echo report to share.
pause
exit /b 1
