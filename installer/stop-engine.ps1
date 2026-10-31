# Stop the Virtual AC3 Encoder engine, control center, and legacy supervisor (matched by command line).
# Used by the uninstaller; safe to run anytime.
Get-CimInstance Win32_Process -Filter "Name='engine.exe' OR Name='control_center.exe' OR Name='wscript.exe'" |
  Where-Object { $_.CommandLine -like '*Virtual AC3*' } |
  ForEach-Object { $_ | Invoke-CimMethod -MethodName Terminate | Out-Null }
