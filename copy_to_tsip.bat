@echo off
rem Copy built plugin DLL to tSIP build output directories (phone subdirectory):
rem Debug_Build and Release_Build - both, regardless of plugin configuration.
rem Used by Makefile (and so build.bat) and MCP.cbp (post-build step).
rem
rem Usage: copy_to_tsip.bat path\to\MCP.dll
rem tSIP application directory can be overridden with TSIP_DIR environment
rem variable, default: ..\tSIP\tSIP (relative to this script).

setlocal

set DLL=%~1
if "%DLL%"=="" (
	echo ERROR: DLL path not specified
	exit /b 1
)
set DLL=%DLL:/=\%
if not exist "%DLL%" (
	echo ERROR: "%DLL%" not found
	exit /b 1
)
if "%TSIP_DIR%"=="" set TSIP_DIR=%~dp0..\tSIP\tSIP

set COPYFAILED=0
for %%B in (Debug_Build Release_Build) do (
	if exist "%TSIP_DIR%\%%B" (
		if not exist "%TSIP_DIR%\%%B\phone" mkdir "%TSIP_DIR%\%%B\phone"
		copy /y "%DLL%" "%TSIP_DIR%\%%B\phone\" >nul
		if errorlevel 1 (
			echo ERROR: failed to copy to %TSIP_DIR%\%%B\phone - is tSIP running with plugin loaded?
			set COPYFAILED=1
		) else (
			echo Updated %TSIP_DIR%\%%B\phone\MCP.dll
		)
	) else (
		echo Skipped %TSIP_DIR%\%%B - directory not found
	)
)
exit /b %COPYFAILED%
