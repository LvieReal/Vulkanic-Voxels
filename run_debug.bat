cd /d "%~dp0"
set PATH=%PATH%;C:\msys64\mingw64\bin
REM Validation layers ON by default for debug runs (--validation). Drop the
REM flag to run without them; any other option can be added on this line.
"build/debug/bin/game.exe" --validation %*
pause
