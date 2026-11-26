@echo off
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo Hay chay scripts\setup.ps1 truoc.
  pause
  exit /b 1
)
echo Mo trinh duyet: http://127.0.0.1:5000
".venv\Scripts\python.exe" backend\run.py
pause

