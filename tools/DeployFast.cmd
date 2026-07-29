@echo off
setlocal EnableDelayedExpansion

rem ============================================================================
rem  DeployFast.cmd — push the Fast-profile build to the places it gets run from.
rem
rem  One portable install and the two sandbox instances used for remote-mirroring
rem  testing (F9/F10/F11/F12), which need to be separate copies: two instances
rem  driving each other have to be two processes with their own qivRemote.ini.
rem
rem  Each destination is attempted independently. A running instance holds a LOCK
rem  on its exe, so a copy failing is the ordinary case, not a reason to stop —
rem  the other two still want updating, and the summary says which to close.
rem ============================================================================

set "SRC=Z:\QIV\fast\QuickImageViewer.exe"

set "DST1=D:\08_PortablePrograms\qIV"
set "DST2=D:\21_sandBox\qivTesting1"
set "DST3=D:\21_sandBox\qivTesting2"

if not exist "%SRC%" (
    echo [ERROR] Source not found:
    echo         %SRC%
    echo         Build the Fast profile first.
    exit /b 1
)

for %%F in ("%SRC%") do echo Source: %%~fF  ^(%%~zF bytes, %%~tF^)
echo.

set /a FAILED=0

call :Deploy "%DST1%"
call :Deploy "%DST2%"
call :Deploy "%DST3%"

echo.
if %FAILED% EQU 0 (
    echo All copies succeeded.
    exit /b 0
)
echo %FAILED% cop^(y^)^(ies^) failed - close those instances and run this again.
exit /b 1

rem ----------------------------------------------------------------------------
rem :Deploy <destination folder>
rem ----------------------------------------------------------------------------
:Deploy
set "DEST=%~1"

if not exist "%DEST%\" (
    mkdir "%DEST%" 2>nul
    if errorlevel 1 (
        echo   [FAIL] %DEST%  - cannot create folder
        set /a FAILED+=1
        goto :eof
    )
    echo   [ .. ] %DEST%  - created
)

rem /Y overwrites without prompting. Redirecting the byte-count chatter keeps the
rem output one line per destination; errorlevel still tells us what happened.
copy /Y "%SRC%" "%DEST%\QuickImageViewer.exe" >nul 2>&1
if errorlevel 1 (
    echo   [FAIL] %DEST%  - copy refused ^(instance running?^)
    set /a FAILED+=1
    goto :eof
)

echo   [ OK ] %DEST%
goto :eof
