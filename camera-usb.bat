@echo off
cd /d "%~dp0"
start "" powershell.exe -NoProfile -STA -WindowStyle Hidden -Command "$ErrorActionPreference = 'Stop'; Add-Type -AssemblyName System.Windows.Forms,System.Drawing; try { Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing -TypeDefinition (Get-Content -LiteralPath 'scripts\CameraUsbWindow.cs' -Raw -Encoding UTF8); [SmartClassroom.CameraUsbWindow]::Run() } catch { [void][System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Smart Classroom - Camera USB') }"
