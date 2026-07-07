@echo off
rem Spusti push_fotapkg.py (vyber .fotapkg.json -> adb push do telefonu).
rem Double-click; netreba zadavat cesty.
cd /d "%~dp0"

rem 1) Znamy PlatformIO python (ctypes staci, tkinter netreba).
set "PYEXE=D:\FkDev\.platformio\python3\python.exe"
if exist "%PYEXE%" (
    "%PYEXE%" "%~dp0push_fotapkg.py"
    goto :done
)

rem 2) py launcher, 3) python z PATH.
where py >nul 2>nul
if %errorlevel%==0 (
    py "%~dp0push_fotapkg.py"
) else (
    python "%~dp0push_fotapkg.py"
)

:done
if %errorlevel% neq 0 pause
