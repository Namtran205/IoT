$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
if (-not (Test-Path -LiteralPath '.venv\Scripts\python.exe')) {
    throw 'Hay chay scripts\setup.ps1 truoc.'
}
& '.\.venv\Scripts\python.exe' backend/run.py

