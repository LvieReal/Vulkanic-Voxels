cd /d "%~dp0"
set PATH=%PATH%;C:\msys64\mingw64\bin
REM Validation layers ON by default for debug runs: the Khronos validation
REM layer is enabled when VV_VALIDATION is set (any value). Set it to nothing
REM (or delete this line) to run without it.
set VV_VALIDATION=1
"build/debug/bin/game.exe"
pause
