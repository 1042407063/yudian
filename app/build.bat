@echo off
rem Delayed expansion is needed to read a counter set inside a ( ) block.
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ---------------------------------------------------------------
rem Toolchain lookup order:
rem   1) MINGW_BIN environment variable
rem   2) D:\toolchains\mingw64\bin      (recommended: ASCII path only)
rem   3) %LOCALAPPDATA%\Mingw64\mingw64\bin  (winget 装的 WinLibs)
rem   4) ..\.toolchain\mingw64\bin
rem   5) g++ already in PATH
rem NOTE: ld.exe cannot handle non-ASCII characters in the *toolchain*
rem       path, so keep MinGW somewhere like D:\toolchains\mingw64.
rem ---------------------------------------------------------------

set "FOUND="
if defined MINGW_BIN if exist "%MINGW_BIN%\g++.exe" set "FOUND=%MINGW_BIN%"
if not defined FOUND if exist "D:\toolchains\mingw64\bin\g++.exe" set "FOUND=D:\toolchains\mingw64\bin"
if not defined FOUND if exist "%LOCALAPPDATA%\Mingw64\mingw64\bin\g++.exe" set "FOUND=%LOCALAPPDATA%\Mingw64\mingw64\bin"
if not defined FOUND if exist "%~dp0..\.toolchain\mingw64\bin\g++.exe" set "FOUND=%~dp0..\.toolchain\mingw64\bin"

if defined FOUND (
    set "PATH=%FOUND%;%PATH%"
) else (
    where g++ >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] g++ not found.
        echo   Put MinGW-w64 under D:\toolchains\mingw64  or set MINGW_BIN.
        exit /b 1
    )
)

if not exist build mkdir build

rem ---------------------------------------------------------------
rem Optional: `build.bat check`
rem Same sources with -Wall -Wextra, report only. Never blocks a build;
rem the point is to see how much noise is there before deciding to tighten.
rem ---------------------------------------------------------------
if /i "%~1"=="check" (
    windres -i src\app.rc -o build\app.res -O coff
    echo [check] compiling with -Wall -Wextra ^(report only^)...
    g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -municode -mwindows -static ^
        -Wl,--subsystem,windows -fno-exceptions -fno-rtti ^
        -Wall -Wextra ^
        src\main.cpp src\ui.cpp src\providers.cpp src\hid.cpp src\selftest.cpp build\app.res ^
        -lhid -lsetupapi -lgdiplus -ldwmapi -lshell32 -luser32 -lgdi32 ^
        -ladvapi32 -lole32 -luuid -lcomctl32 ^
        -o build\yudian.check.exe 2> build\warnings.txt
    set "NW=0"
    rem Absolute paths and no pipe: Git-for-Windows also ships a "find" and a
    rem quoted path inside for /f gets re-parsed by cmd.
    "%SystemRoot%\System32\findstr.exe" /C:"warning:" build\warnings.txt > build\warnonly.txt
    for /f %%c in ('%SystemRoot%\System32\find.exe /c /v "" ^< build\warnonly.txt') do set "NW=%%c"
    del /q build\warnonly.txt >nul 2>nul
    echo [check] !NW! warning^(s^). Full log: build\warnings.txt
    exit /b 0
)

echo [1/4] resources...
windres -i src\app.rc -o build\app.res -O coff
if errorlevel 1 exit /b 1

rem -mwindows sets the GUI subsystem, and --subsystem,windows states it again
rem explicitly. A CONSOLE-subsystem build pops up an empty black window on
rem double-click -- that regression has bitten before, so it is verified below.
echo [2/4] compile + link...
g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -municode -mwindows -static -s ^
    -Wl,--subsystem,windows ^
    -fno-exceptions -fno-rtti ^
    src\main.cpp src\ui.cpp src\providers.cpp src\hid.cpp src\selftest.cpp build\app.res ^
    -lhid -lsetupapi -lgdiplus -ldwmapi -lshell32 -luser32 -lgdi32 ^
    -ladvapi32 -lole32 -luuid -lcomctl32 ^
    -o build\yudian.exe
if errorlevel 1 exit /b 1

rem PE subsystem must be 00000002 (Windows GUI), not 00000003 (Windows CUI).
set "SUBSYS="
for /f "tokens=2" %%a in ('objdump -p build\yudian.exe ^| findstr /B "Subsystem"') do set "SUBSYS=%%a"
if not "%SUBSYS%"=="00000002" (
    echo [ERROR] PE subsystem = %SUBSYS% , expected 00000002 ^(Windows GUI^).
    echo         Double-clicking would open an empty console window.
    exit /b 1
)

rem Pure-function self test. A parsing regression must fail here instead of
rem showing up as a wrong battery percentage on someone's desk.
rem start /wait is required: cmd does not wait for GUI-subsystem programs.
echo [3/4] self test...
if exist build\selftest.txt.rc del /q build\selftest.txt.rc >nul 2>nul
start /wait "" build\yudian.exe --selftest build\selftest.txt
set "STRC="
if exist build\selftest.txt.rc set /p STRC=<build\selftest.txt.rc
if not "%STRC%"=="0" (
    echo.
    if exist build\selftest.txt type build\selftest.txt
    echo [ERROR] self test failed or did not run. See build\selftest.txt
    rem Do not leave a broken build where a double-click will find it.
    if exist build\yudian.exe ren build\yudian.exe yudian.broken.exe
    echo         This build is renamed to build\yudian.broken.exe - do not use it.
    exit /b 1
)

echo [4/4] done. subsystem=%SUBSYS% (GUI)
for %%F in (build\yudian.exe) do echo   %%~fF  %%~zF bytes
