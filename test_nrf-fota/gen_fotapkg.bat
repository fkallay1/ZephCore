@echo off
setlocal enabledelayedexpansion
rem Spusti gen_fotapkg.py interaktivne: opyta sa na device + 2 buildy a vyrobi
rem <old>-<new>.<device>.fotapkg.json (+ .rev) do fotapkg_json/.
rem Double-click; vsetky cesty su relativne k tomuto .bat, netreba zadavat cesty
rem ani spoliehat sa na PATH (rovnaky princip ako push_fotapkg.bat).
cd /d "%~dp0"

rem gen_fotapkg.py potrebuje pyserial -> PlatformIO penv python (NIE python3,
rem ktory pouziva push_fotapkg.bat - ten serial nema). Fallback: py / python.
set "PYEXE="
if exist "D:\FkDev\.platformio\penv\Scripts\python.exe" set "PYEXE=D:\FkDev\.platformio\penv\Scripts\python.exe"

rem Slovenske znaky a sipku '->' v stdoute gen_fotapkg.py tlac v UTF-8.
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

if not exist "builds" (
    echo CHYBA: adresar builds\ neexistuje - najprv buildni FOTA env.
    pause & exit /b 1
)

rem --- Zoznam dostupnych zariadeni v builds\ ---
set "DEVLIST= "
for /f "tokens=1 delims=." %%F in ('dir /b builds\*.fw_*.bin 2^>nul') do (
    echo !DEVLIST! | find /i " %%F " >nul || set "DEVLIST=!DEVLIST!%%F "
)
echo Dostupne zariadenia:!DEVLIST!

rem Default device = prve v zozname (zvycajne promicro/sensecap).
for /f "tokens=1" %%D in ("!DEVLIST!") do set "DEVICE=%%D"
set /p "DEVICE=Device [!DEVICE!]: "

rem --- Zoznam buildov pre zvolene zariadenie (od najnovsieho) ---
set "FOUND="
echo Dostupne buildy pre %DEVICE%:
for /f "tokens=3 delims=._" %%N in ('dir /b /o-n builds\%DEVICE%.fw_*.bin 2^>nul') do (
    set "FOUND=1"
    echo    %%N
)
if not defined FOUND (
    echo CHYBA: pre '%DEVICE%' nie su v builds\ ziadne .bin.
    pause & exit /b 1
)

echo.
echo Tip: necha oba prazdne = 2 najnovsie buildy (--auto).
set /p "OLD=Stary build #: "
set /p "NEW=Novy build #: "

if "%OLD%"=="" if "%NEW%"=="" (
    call :run --auto --device %DEVICE%
    goto :end
)

set "OLDBIN=builds\%DEVICE%.fw_%OLD%.bin"
set "NEWBIN=builds\%DEVICE%.fw_%NEW%.bin"
if not exist "%OLDBIN%" ( echo CHYBA: %OLDBIN% neexistuje. & pause & exit /b 1 )
if not exist "%NEWBIN%" ( echo CHYBA: %NEWBIN% neexistuje. & pause & exit /b 1 )

call :run --device %DEVICE% --old "%OLDBIN%" --new "%NEWBIN%"

:end
echo.
if %errorlevel% neq 0 ( echo Skoncilo s chybou ^(rc=%errorlevel%^). & pause )
exit /b %errorlevel%

rem --- spusti gen_fotapkg.py s danymi argumentmi cez najlepsi dostupny python ---
:run
if defined PYEXE (
    "%PYEXE%" "%~dp0gen_fotapkg.py" %*
    exit /b %errorlevel%
)
where py >nul 2>nul
if %errorlevel%==0 (
    py "%~dp0gen_fotapkg.py" %*
) else (
    python "%~dp0gen_fotapkg.py" %*
)
exit /b %errorlevel%
