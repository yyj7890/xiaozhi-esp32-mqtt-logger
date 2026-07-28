$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSCommandPath
$tailscaleCommand = Get-Command tailscale -ErrorAction SilentlyContinue
$tailscale = if ($tailscaleCommand) { $tailscaleCommand.Source } else { 'D:\gj\Tailscale\tailscale.exe' }
if (-not (Test-Path $tailscale)) { throw 'Tailscale executable was not found.' }
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
$python = if ($pythonCommand) { $pythonCommand.Source } else { 'D:\Anaconda3\python.exe' }
if (-not (Test-Path $python)) { throw 'Python executable was not found.' }
$tailIp = (& $tailscale ip -4 | Select-Object -First 1).Trim()
if (-not $tailIp) { throw 'Tailscale is not connected.' }

Set-Location $root
if (-not (Test-Path "$root\apps.json")) { Copy-Item "$root\apps.example.json" "$root\apps.json" }

@"
Laptop MCP agent URL: http://${tailIp}:8765/mcp
Use this value only in the NAS bridge.env as PC_MCP_URL.
"@ | Set-Content -Path "$root\agent-status.txt" -Encoding utf8

& $python "$root\agent.py" --host $tailIp --port 8765
