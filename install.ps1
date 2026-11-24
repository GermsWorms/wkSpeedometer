# Copies wkSpeedometer.dll (and wkSpeedometer.ini, if there is none yet) into the Worms Armageddon folder, or removes
# the module with -Remove. WA loads it at its next start when Options > Advanced > "Load WormKit modules" is on. Without
# -Game it looks in Steam's and GOG's usual folders.
param([switch]$Remove, [string]$Game = '')
$ErrorActionPreference = 'Stop'
if (-not $Game) {
    $Game = @('C:\Program Files (x86)\Steam\steamapps\common\Worms Armageddon',
              'C:\Program Files (x86)\GOG Galaxy\Games\Worms Armageddon',
              'C:\GOG Games\Worms Armageddon') | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'WA.exe') } | Select-Object -First 1
    if (-not $Game) { throw 'WA.exe not found in the Steam or GOG folders (use -Game <your WA folder>)' }
}
$target = Join-Path $Game 'wkSpeedometer.dll'
if ($Remove) {
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target; "removed $target" } else { 'not installed' }
    return
}
if (-not (Test-Path -LiteralPath (Join-Path $Game 'WA.exe'))) { throw "WA.exe not found in $Game (use -Game <your WA folder>)" }
$hash = (Get-FileHash -LiteralPath (Join-Path $Game 'WA.exe')).Hash
if ($hash -ne 'D6C449C75916A0941D1163AFC2A91F1E72A31600609BB9B766F2302CA80164EB') {
    "Note: this WA.exe is not Steam's 3.8.1 file. When WA starts, the module checks that the game's code is 3.8.1's (GOG's"
    '3.8.1 is the same game) and logs whether it runs: %LOCALAPPDATA%\wkSpeedometer\wkSpeedometer.log'
}
$dll = Join-Path $PSScriptRoot 'wkSpeedometer.dll'      # (your own build, else the released file)
if (-not (Test-Path -LiteralPath $dll)) { $dll = Join-Path $PSScriptRoot 'release\wkSpeedometer.dll' }
if (-not (Test-Path -LiteralPath $dll)) { throw "wkSpeedometer.dll not found next to install.ps1 or in release\ (run .\build.ps1 first)" }
Copy-Item -LiteralPath $dll -Destination $target -Force
"installed $target (WA loads it at its next start)"
$ini = Join-Path $Game 'wkSpeedometer.ini'
if (-not (Test-Path -LiteralPath $ini)) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'wkSpeedometer.ini') -Destination $ini }
$opt = Get-ItemProperty -Path 'HKCU:\Software\Team17SoftwareLTD\WormsArmageddon\Options' -ErrorAction SilentlyContinue
if (-not $opt -or $opt.LoadWormKitModules -ne 1) { 'Turn on Options > Advanced > "Load WormKit modules" in WA (if it is not on yet)' }
