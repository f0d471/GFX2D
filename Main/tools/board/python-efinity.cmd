@echo off
rem Launch a script with the Efinity bundled Python.
rem
rem Why this exists: this machine has no standalone Windows Python on PATH.
rem Efinity's python311 needs PYTHONHOME set (without it, "No module named
rem 'encodings'"), while the vendor programmer tools need PYTHONHOME *unset*
rem so they can run their own setup.bat. This wrapper sets PYTHONHOME for its
rem own process only; callers that need a clean environment for a child
rem (see tools/board/board_run.py program_bitstream) strip it themselves.
rem
rem Keep this file ASCII-only: cmd.exe reads .cmd in the OEM code page, so
rem non-ASCII comments here turn into garbage commands.
rem
rem Usage: tools\board\python-efinity.cmd <script> [args...]
rem        tools\board\python-efinity.cmd -c "import serial; print(serial.__version__)"
setlocal
set "EFINITY_ROOT=C:\Efinity\2026.1"
if not exist "%EFINITY_ROOT%\python311\bin\python.exe" (
    echo python-efinity: missing %EFINITY_ROOT%\python311\bin\python.exe 1>&2
    exit /b 127
)
set "PYTHONHOME=%EFINITY_ROOT%\python311"
"%EFINITY_ROOT%\python311\bin\python.exe" %*
exit /b %ERRORLEVEL%
