# Builds the oracle install: a private copy of Hollow Knight that runs the HKOracle mod, next to (never
# inside) your Steam install. Idempotent: rerun it to refresh the mod or repair the install.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_oracle_install.ps1 [-Game <HK dir>] [-Dst <dir>]
#                                                                           [-ModdingApi <zip>] [-Mod <dir>]
#
# -Game        the Hollow Knight install to copy from; default: found through Steam's library folders.
# -Dst         where the oracle install goes; default $env:HKRL_GAME, else <repo>\game.
# -ModdingApi  the Modding API zip (ModdingApiWin.zip, github.com/hk-modding/api/releases), unpacked into
#              the copy only. Not needed when the game already has the API (installed with Scarab).
# -Mod         the folder holding HKOracle.dll; default <repo>\dist\HKOracle (the release download),
#              else <repo>\oracle\bin\Release (your own build).
#
# What keeps the copy away from your real game:
# - Steam: the game only talks to Steam when Plugins\x86_64\steam_api64.dll exists
#   (SteamOnlineSubsystem.IsPackaged, DesktopPlatform.CreateOnlineSubsystem). The copy has its own Plugins
#   without it, so an oracle launch never starts Steam, shows as playing, or syncs Steam Cloud.
# - Saves and settings: the game keeps them in LocalLow\<company>\Hollow Knight and the registry key
#   HKCU\Software\<company>\Hollow Knight. The copy's globalgamemanagers and app.info name the company
#   "hkrl oracle" instead of "Team Cherry" (same length, so no offsets move), so the copy never reads or
#   writes your saves, settings or mod settings.
# - Files the game writes in its data folder (Config.ini, ConfigManager.Init) are copies, never hardlinks
#   into the Steam install. Only large, read-only asset files are hardlinked (same volume) or copied.
# - No junctions into the Steam install: its small folders (MonoBleedingEdge, Resources, StreamingAssets)
#   are copied, so no way of deleting the copy can reach into the game. Deleting a hardlink only removes
#   the copy's name for the file.
param(
  [string]$Game = "",
  [string]$Dst = "",
  [string]$ModdingApi = "",
  [string]$Mod = ""
)
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Company = "hkrl oracle"                         # must stay 11 bytes: it overwrites "Team Cherry" in place

function Find-HollowKnight {
  $roots = @()
  try { $sp = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -Name SteamPath).SteamPath } catch { $sp = $null }
  if ($sp) {
    $roots += $sp
    $vdf = Join-Path $sp "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
      $roots += [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
    }
  }
  $roots += "C:\Program Files (x86)\Steam"
  foreach ($r in ($roots | Select-Object -Unique)) {
    $g = Join-Path $r "steamapps\common\Hollow Knight"
    if (Test-Path (Join-Path $g "hollow_knight.exe")) { return (Resolve-Path $g).Path }
  }
  return $null
}

# A hardlink shares its bytes with the Steam install: only ever unlink it, never write through it.
function Copy-Fresh($s, $d) { if (Test-Path $d) { Remove-Item $d -Force }; Copy-Item $s $d }
function Link-File($s, $d) {
  if (Test-Path $d) { return }
  try { New-Item -ItemType HardLink -Path $d -Target $s | Out-Null } catch { Copy-Item $s $d }
}
# rmdir removes a junction itself, never what it points to (an install made before these copies had them).
function Remove-Junction($d) { if ((Test-Path $d) -and (Get-Item $d -Force).LinkType -eq "Junction") { cmd /c rmdir "$d" | Out-Null } }
function Copy-Dir($s, $d) { Remove-Junction $d; robocopy $s $d /E /NFL /NDL /NJH /NJS /NP | Out-Null }

function Set-Company($file) {
  if (@(fsutil hardlink list $file).Count -ne 1) { throw "$file is hardlinked to another install: not patching it" }
  $b = [IO.File]::ReadAllBytes($file)
  $text = [Text.Encoding]::GetEncoding(28591).GetString($b)          # latin-1: one char per byte
  $n = ([regex]::Matches($text, "Team Cherry")).Count
  if ($n -eq 0 -and $text.Contains($Company)) { return }
  if ($n -ne 1) { throw "$file names 'Team Cherry' $n times, expected once: not patching it" }
  $i = $text.IndexOf("Team Cherry")
  [Text.Encoding]::ASCII.GetBytes($Company).CopyTo($b, $i)
  [IO.File]::WriteAllBytes($file, $b)
}

# ---- the source game
if (-not $Game) { $Game = Find-HollowKnight }
if (-not $Game -or -not (Test-Path (Join-Path $Game "hollow_knight.exe"))) {
  throw "Hollow Knight not found. Pass -Game '<folder with hollow_knight.exe>'."
}
$srcData = Join-Path $Game "hollow_knight_Data"
$srcManaged = Join-Path $srcData "Managed"
$asm = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes((Join-Path $srcManaged "Assembly-CSharp.dll")))
$version = ([regex]::Match($asm, '1\.\d+\.\d+\.\d+')).Value
"[game] $Game (version $version)"
if ($version -ne "1.5.78.11833") { Write-Warning "HKOracle is built for Hollow Knight 1.5.78.11833; this is $version." }
$hasApi = Test-Path (Join-Path $srcManaged "MMHOOK_Assembly-CSharp.dll")
if (-not $hasApi -and -not $ModdingApi) {
  throw ("The Modding API is not installed. Either install it into the game with Scarab " +
         "(https://github.com/fifty-six/Scarab), or download ModdingApiWin.zip for 1.5.78 from " +
         "https://github.com/hk-modding/api/releases and pass -ModdingApi <zip> (the copy gets it, your game stays vanilla).")
}

# ---- the destination
if (-not $Dst) { $Dst = if ($env:HKRL_GAME) { $env:HKRL_GAME } else { Join-Path $Repo "game" } }
$gameFull = [IO.Path]::GetFullPath($Game).TrimEnd("\") + "\"
$dstFull = [IO.Path]::GetFullPath($Dst).TrimEnd("\") + "\"
if ($dstFull.StartsWith($gameFull, [StringComparison]::OrdinalIgnoreCase) -or
    $gameFull.StartsWith($dstFull, [StringComparison]::OrdinalIgnoreCase)) {
  throw "-Dst ${Dst} overlaps the game folder ${Game}; the copy must live outside it"
}
New-Item -ItemType Directory -Force $Dst | Out-Null
$Dst = (Resolve-Path $Dst).Path
$running = Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($Dst, [StringComparison]::OrdinalIgnoreCase) }
if ($running) { throw "Game instances from $Dst are running ($($running.Name -join ', ')). Stop them first." }
$dstData = Join-Path $Dst "oracle_Data"
New-Item -ItemType Directory -Force $dstData | Out-Null

# ---- a one-time backup of your real saves, before this install ever runs
$real = Join-Path $env:USERPROFILE "AppData\LocalLow\Team Cherry\Hollow Knight"
$backup = Join-Path $Dst "save_backup"
$saves = @(Get-ChildItem $real -Filter "user*.dat*" -File -ErrorAction SilentlyContinue)
if ($saves.Count -and -not (Test-Path $backup)) {
  New-Item -ItemType Directory $backup | Out-Null
  $saves | Copy-Item -Destination $backup
  "[saves] backed up $($saves.Count) save files from $real to $backup"
}

# ---- the executable (the data folder is named after it: oracle.exe -> oracle_Data)
Copy-Item (Join-Path $Game "hollow_knight.exe") (Join-Path $Dst "oracle.exe") -Force
foreach ($f in @("UnityPlayer.dll", "UnityCrashHandler64.exe")) { Link-File (Join-Path $Game $f) (Join-Path $Dst $f) }
Copy-Dir (Join-Path $Game "MonoBleedingEdge") (Join-Path $Dst "MonoBleedingEdge")
if (Test-Path (Join-Path $Dst "steam_appid.txt")) { Remove-Item (Join-Path $Dst "steam_appid.txt") -Force }

# ---- the data folder
$linked = 0; $copied = 0
Get-ChildItem $srcData | ForEach-Object {
  $d = Join-Path $dstData $_.Name
  if ($_.Name -in @("Managed", "Plugins")) { return }
  if ($_.PSIsContainer) { Copy-Dir $_.FullName $d; return }
  if ($_.Length -ge 1MB -and $_.Name -ne "globalgamemanagers") { Link-File $_.FullName $d; $linked++ }
  else { Copy-Fresh $_.FullName $d; $copied++ }
}
Set-Company (Join-Path $dstData "globalgamemanagers")
Set-Company (Join-Path $dstData "app.info")
$how = if ((Split-Path -Qualifier $Dst) -eq (Split-Path -Qualifier $Game)) { "hardlinked" } else { "copied (another drive than the game)" }
"[data] $linked asset files $how, $copied small files copied; company '$Company'"

# Plugins: a real folder without Steam's (or GOG Galaxy's) native library.
$plugins = Join-Path $dstData "Plugins"
Remove-Junction $plugins
robocopy (Join-Path $srcData "Plugins") $plugins /E /XF "steam_api*.dll" "Galaxy*.dll" /NFL /NDL /NJH /NJS /NP | Out-Null
Get-ChildItem $plugins -Recurse -Include "steam_api*.dll", "Galaxy*.dll" | Remove-Item -Force
"[steam] no steam_api64.dll in $plugins"

# Managed: a real copy; Mods holds HKOracle only.
$managed = Join-Path $dstData "Managed"
robocopy $srcManaged $managed /E /XD Mods /NFL /NDL /NJH /NJS /NP | Out-Null
if ($ModdingApi) { Expand-Archive $ModdingApi -DestinationPath $managed -Force; "[api] unpacked $ModdingApi" }
if (-not (Test-Path (Join-Path $managed "MMHOOK_Assembly-CSharp.dll"))) { throw "No Modding API in $managed." }

if (-not $Mod) {
  $Mod = @((Join-Path $Repo "dist\HKOracle"), (Join-Path $Repo "oracle\bin\Release")) |
    Where-Object { Test-Path (Join-Path $_ "HKOracle.dll") } | Select-Object -First 1
}
$mods = Join-Path $managed "Mods"
New-Item -ItemType Directory -Force (Join-Path $mods "HKOracle") | Out-Null
if ($Mod) {
  foreach ($f in @("HKOracle.dll", "HKOracle.pdb", "Newtonsoft.Json.dll", "websocket-sharp.dll")) {
    if (Test-Path (Join-Path $Mod $f)) { Copy-Item (Join-Path $Mod $f) (Join-Path $mods "HKOracle") -Force }
  }
  "[mod] HKOracle from $Mod"
} else {
  Write-Warning "No HKOracle.dll found (run setup.ps1, or build oracle\HKOracle.csproj). The install has no mod yet."
}
$other = @(Get-ChildItem $mods -Directory | Where-Object { $_.Name -notin @("HKOracle", "Disabled") })
if ($other) { Write-Warning "Other mods in $mods change the game: $($other.Name -join ', ')" }
"[done] $Dst"
