@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -Command "$name='DarwinDbC-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'); Set-Content -Encoding Ascii -Path 'current-capture.txt' -Value ($name+'.log'); $process=Start-Process -FilePath '.\darwindbc-receive.exe' -ArgumentList @('--output',($name+'.log')) -RedirectStandardOutput ($name+'.console.txt') -RedirectStandardError ($name+'.status.txt') -NoNewWindow -Wait -PassThru; exit $process.ExitCode"
