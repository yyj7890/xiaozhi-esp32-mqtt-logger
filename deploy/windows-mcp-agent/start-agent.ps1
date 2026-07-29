$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSCommandPath
$tailscaleCommand = Get-Command tailscale -ErrorAction SilentlyContinue
$tailscale = if ($tailscaleCommand) { $tailscaleCommand.Source } else { 'D:\gj\Tailscale\tailscale.exe' }
if (-not (Test-Path $tailscale)) { throw 'Tailscale executable was not found.' }
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
$python = if ($pythonCommand) { $pythonCommand.Source } else { 'D:\Anaconda3\python.exe' }
if (-not (Test-Path $python)) { throw 'Python executable was not found.' }
# At Windows logon Tailscale can need a few seconds to reconnect. Do not let a
# one-time startup race leave the laptop MCP agent offline for the whole login.
$tailIp = ''
$deadline = (Get-Date).AddSeconds(120)
do {
    try {
        $tailIp = ((& $tailscale ip -4 2>$null | Select-Object -First 1) -as [string]).Trim()
    } catch {
        $tailIp = ''
    }
    if ($tailIp) { break }
    Start-Sleep -Seconds 3
} while ((Get-Date) -lt $deadline)
if (-not $tailIp) { throw 'Tailscale did not connect within 120 seconds.' }

Set-Location $root
if (-not (Test-Path "$root\apps.json")) { Copy-Item "$root\apps.example.json" "$root\apps.json" }

@"
Laptop MCP agent URL: http://${tailIp}:8765/mcp
Use this value only in the NAS bridge.env as PC_MCP_URL.
"@ | Set-Content -Path "$root\agent-status.txt" -Encoding utf8

& $python "$root\agent.py" --host $tailIp --port 8765
