@echo off
rem Start de FNK0104N Bin Maker (Python + tkinter). ASCII-only.
cd /d "%~dp0"
where pyw >nul 2>nul
if %errorlevel%==0 (
  start "" pyw "%~dp0fnk0104n_bin_maker.py"
  exit /b
)
where pythonw >nul 2>nul
if %errorlevel%==0 (
  start "" pythonw "%~dp0fnk0104n_bin_maker.py"
  exit /b
)
python "%~dp0fnk0104n_bin_maker.py"
pause
