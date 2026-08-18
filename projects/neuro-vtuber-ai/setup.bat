@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if errorlevel 1 (
  echo Python launcher was not found. Install Python 3.11 or newer first.
  pause
  exit /b 1
)

if not exist .venv (
  py -m venv .venv
)

call .venv\Scripts\activate.bat
python -m pip install --upgrade pip
python -m pip install -r requirements.txt

if not exist .env copy .env.example .env >nul

echo.
echo Setup complete.
echo Next: install Ollama, then run: ollama pull gemma3
echo After that, double-click run.bat
pause
