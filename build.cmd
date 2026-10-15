@echo off
rem cmd.exe shim for build.ps1: build.cmd [iso|clean] [-BuildDir d] [-ExtraCFlags "-DFOO=1"] ...
rem (if a value that starts with "-" is reported as an unknown parameter, write -ExtraCFlags:"-DFOO=1")
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
