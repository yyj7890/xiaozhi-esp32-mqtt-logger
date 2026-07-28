$ErrorActionPreference = 'Stop'

# Run this script once as administrator. It deliberately creates only the
# three named, fixed-executable tasks below; it does not accept input.
$userId = "$env:USERDOMAIN\$env:USERNAME"
$config = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'apps.json') -Raw -Encoding utf8 | ConvertFrom-Json
$valorant = $config.allowed_apps.psobject.Properties | Where-Object { $_.Value.path -match '\\ACLOS\\' } | Select-Object -First 1
$league = $config.allowed_apps.psobject.Properties | Where-Object { $_.Value.path -match '\\LeagueClient\\LeagueClient\.exe$' } | Select-Object -First 1
if (-not $valorant -or -not $league) {
    throw 'The approved VALORANT or League of Legends launcher was not found in apps.json.'
}
$games = @(
    @{ Name = 'Xiaozhi Game - WeGame'; Path = $config.allowed_apps.WeGame.path },
    @{ Name = 'Xiaozhi Game - VALORANT'; Path = $valorant.Value.path },
    @{ Name = 'Xiaozhi Game - League of Legends'; Path = $league.Value.path }
)

foreach ($game in $games) {
    if (-not (Test-Path -LiteralPath $game.Path -PathType Leaf)) {
        throw "Game launcher was not found: $($game.Path)"
    }

    $action = New-ScheduledTaskAction -Execute $game.Path
    $principal = New-ScheduledTaskPrincipal -UserId $userId -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $game.Name -Action $action -Principal $principal -Settings $settings -Description 'Fixed owner-approved XiaoZhi game launcher.' -Force | Out-Null
    Write-Output "Created: $($game.Name)"
}
