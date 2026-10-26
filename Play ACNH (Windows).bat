@echo off
rem Double-click in Explorer to play on Windows: opens the launcher (building it the first time, a few minutes),
rem which sets the game up from your game folder the first time and starts it afterwards (README, Installation).
call "%~dp0tools\launcher.bat" %*
