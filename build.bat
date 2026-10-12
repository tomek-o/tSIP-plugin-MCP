@echo off
rem Build tSIP MCP server plugin (MCP.dll) using Makefile
rem
rem Usage: build.bat [release|debug|clean|testhost]
rem   default: release -> bin\Release\MCP.dll
rem
rem MinGW location can be overridden with MINGW_BIN environment variable,
rem default: Code::Blocks 25.03 bundled MinGW (GCC 14.2, 32-bit).
rem
rem Built DLL is copied to tSIP Debug_Build\phone and Release_Build\phone;
rem tSIP application directory can be overridden with TSIP_DIR environment
rem variable, default: ..\tSIP\tSIP (relative to this script).

setlocal

if "%MINGW_BIN%"=="" set MINGW_BIN=G:\zrzut\codeblocks-25.03mingw-32bit-nosetup\MinGW\bin
if "%TSIP_DIR%"=="" set TSIP_DIR=%~dp0..\tSIP\tSIP

if not exist "%MINGW_BIN%\mingw32-make.exe" (
	echo ERROR: mingw32-make.exe not found in "%MINGW_BIN%"
	echo Set MINGW_BIN to MinGW bin directory.
	exit /b 1
)

set PATH=%MINGW_BIN%;%PATH%
cd /d "%~dp0"

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=release

if /i "%CONFIG%"=="clean" (
	mingw32-make clean
	exit /b %errorlevel%
)
if /i "%CONFIG%"=="testhost" (
	rem test host for MCP protocol tests without tSIP, see test\TestHost.cpp
	mingw32-make all testhost
	exit /b %errorlevel%
)
if /i "%CONFIG%"=="release" (
	set OUTDIR=bin\Release
) else if /i "%CONFIG%"=="debug" (
	set OUTDIR=bin\Debug
) else (
	echo ERROR: unknown configuration "%CONFIG%", use release, debug or clean
	exit /b 1
)

echo Building %CONFIG% ...
mingw32-make CONFIG=%CONFIG%
if errorlevel 1 (
	echo ERROR: build failed
	exit /b 1
)

echo.
echo Exports (tSipPluginInit expected without decoration):
objdump -p %OUTDIR%\MCP.dll | findstr /r /c:"tSipPluginInit"
if errorlevel 1 (
	echo ERROR: tSipPluginInit export not found
	exit /b 1
)

echo.
echo Imported DLLs (no MinGW runtime DLLs expected):
objdump -p %OUTDIR%\MCP.dll | findstr /c:"DLL Name"
objdump -p %OUTDIR%\MCP.dll | findstr /i /c:"libstdc++" /c:"libgcc_s" /c:"libwinpthread" >nul
if not errorlevel 1 (
	echo ERROR: depends on MinGW runtime DLL - check static linking
	exit /b 1
)

echo.
echo OK: %OUTDIR%\MCP.dll

rem Update plugin in tSIP build output directories (phone subdirectory),
rem only for build directories that exist
echo.
set COPYFAILED=0
for %%B in (Debug_Build Release_Build) do (
	if exist "%TSIP_DIR%\%%B" (
		if not exist "%TSIP_DIR%\%%B\phone" mkdir "%TSIP_DIR%\%%B\phone"
		copy /y "%OUTDIR%\MCP.dll" "%TSIP_DIR%\%%B\phone\" >nul
		if errorlevel 1 (
			echo ERROR: failed to copy to %TSIP_DIR%\%%B\phone - is tSIP running?
			set COPYFAILED=1
		) else (
			echo Updated %TSIP_DIR%\%%B\phone\MCP.dll
		)
	) else (
		echo Skipped %TSIP_DIR%\%%B - directory not found
	)
)
if "%COPYFAILED%"=="1" exit /b 1

echo Enable plugin in tSIP Settings / Plugins (if not enabled yet).
exit /b 0
