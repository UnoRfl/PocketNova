# Removes the Pocket Nova Panel (keeps your settings unless -All is given).
param([switch]$All)
$root = Join-Path $env:USERPROFILE 'PocketNovaPanel'

Get-CimInstance Win32_Process -Filter "Name='pythonw.exe' OR Name='python.exe'" -ErrorAction SilentlyContinue |
  Where-Object { $_.CommandLine -like '*pocketnova_panel.py*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Remove-Item (Join-Path ([Environment]::GetFolderPath('Startup')) 'Pocket Nova Panel.lnk') -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'Pocket Nova Panel.lnk') -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Pocket Nova.lnk') -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName 'Pocket Nova Watchdog' -Confirm:$false -ErrorAction SilentlyContinue
if ($All) { Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue }
else { Get-ChildItem $root -Exclude 'settings.json' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force }
Write-Host 'Pocket Nova Panel removed.'
