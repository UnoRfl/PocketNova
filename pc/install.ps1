# Installs the Pocket Nova Panel for this Windows user.
#  - copies the app to %USERPROFILE%\PocketNovaPanel
#  - starts it with Windows (Startup folder shortcut, runs hidden)
#  - adds "Pocket Nova Panel" to the Start menu (opens the panel)
#  - starts the background watcher now
# Run again any time to update. Uninstall with uninstall.ps1.

$ErrorActionPreference = 'Stop'
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$root    = Join-Path $env:USERPROFILE 'PocketNovaPanel'
$appDir  = $root
$fwDir   = Join-Path (Split-Path -Parent $here) 'firmware\PocketNova'

# Use the REAL pythonw.exe, not the WindowsApps alias: the alias runs Python
# inside an app sandbox where AppData is a private copy, which hides the
# Arduino tools from the "Update firmware" button.
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw 'Python was not found. Install Python 3 first.' }
$pyw = (& python -c "import sys,os;print(os.path.join(os.path.dirname(sys.executable),'pythonw.exe'))").Trim()
if (-not (Test-Path $pyw)) { throw "pythonw.exe not found next to python ($pyw)" }

Write-Host 'Checking pyserial...'
& python -c "import serial" 2>$null
if ($LASTEXITCODE -ne 0) { & python -m pip install --user --disable-pip-version-check pyserial==3.5 }

# Optional: lets the panel reach Pocket Nova over Bluetooth when no cable is plugged in.
Write-Host 'Checking bleak (Bluetooth)...'
& python -c "import bleak" 2>$null
if ($LASTEXITCODE -ne 0) { & python -m pip install --user --disable-pip-version-check bleak==3.0.2 }
if ($LASTEXITCODE -ne 0) { Write-Host 'Bluetooth library not installed: the panel will work over USB only.' }

# Stop a running copy so its files can be replaced.
Get-CimInstance Win32_Process -Filter "Name='pythonw.exe' OR Name='python.exe'" -ErrorAction SilentlyContinue |
  Where-Object { $_.CommandLine -like '*pocketnova_panel.py*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

New-Item -ItemType Directory -Force $appDir | Out-Null
Copy-Item (Join-Path $here 'pocketnova_panel.py') $appDir -Force
Copy-Item (Join-Path $here 'panel.html') $appDir -Force
if (-not (Test-Path (Join-Path $here 'PocketNova.ico'))) { & python (Join-Path $here 'make_icon.py') }
Copy-Item (Join-Path $here 'PocketNova.ico') $appDir -Force
Copy-Item (Join-Path $here 'icon.png') $appDir -Force
$icon = Join-Path $appDir 'PocketNova.ico'

# Remember where the firmware source lives (for "Update firmware").
$settingsPath = Join-Path $root 'settings.json'
$settings = @{}
if (Test-Path $settingsPath) {
  try { (Get-Content $settingsPath -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $settings[$_.Name] = $_.Value } } catch {}
}
$settings['firmwareDir'] = $fwDir
($settings | ConvertTo-Json -Depth 5) | Set-Content $settingsPath -Encoding UTF8

$script = Join-Path $appDir 'pocketnova_panel.py'
$shell  = New-Object -ComObject WScript.Shell

$startup = Join-Path ([Environment]::GetFolderPath('Startup')) 'Pocket Nova Panel.lnk'
$s = $shell.CreateShortcut($startup)
$s.TargetPath = $pyw; $s.Arguments = "`"$script`""; $s.WorkingDirectory = $appDir
$s.Description = 'Pocket Nova Panel (opens when Pocket Nova is plugged in)'; $s.IconLocation = "$icon,0"; $s.Save()

$menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Pocket Nova Panel.lnk'
$m = $shell.CreateShortcut($menu)
$m.TargetPath = $pyw; $m.Arguments = "`"$script`" --open"; $m.WorkingDirectory = $appDir
$m.Description = 'Open the Pocket Nova Panel'; $m.IconLocation = "$icon,0"; $m.Save()

# Desktop shortcut with Nova's face.
$desk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Pocket Nova.lnk'
$d = $shell.CreateShortcut($desk)
$d.TargetPath = $pyw; $d.Arguments = "`"$script`" --open"; $d.WorkingDirectory = $appDir
$d.Description = 'Open the Pocket Nova Panel'; $d.IconLocation = "$icon,0"; $d.Save()

# Watchdog: every 5 minutes, start the background program if it isn't running.
# (A second copy exits straight away, so this is harmless when it's already up.)
$action  = New-ScheduledTaskAction -Execute $pyw -Argument "`"$script`"" -WorkingDirectory $appDir
$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 5)
$tset    = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'Pocket Nova Watchdog' -Action $action -Trigger $trigger -Settings $tset -Description 'Keeps the Pocket Nova Panel running' -Force | Out-Null

Start-Process -FilePath $pyw -ArgumentList "`"$script`"" -WorkingDirectory $appDir
Write-Host "Installed to $appDir"
Write-Host 'The panel opens by itself whenever Pocket Nova is plugged in.'
Write-Host 'Open it any time with the Pocket Nova shortcut on your desktop or in the Start menu.'
