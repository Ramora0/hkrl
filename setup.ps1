# One-command setup: the Python environment, the prebuilt binaries of this commit's release, and optionally
# the oracle game install. Rerun it any time; finished steps are skipped.
#
#   powershell -ExecutionPolicy Bypass -File setup.ps1                  # train and evaluate in the sim
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -Game            # + the real game (evals, recording)
#
# -Game             also build the oracle install (tools\make_oracle_install.ps1; needs Hollow Knight
#                   1.5.78 and the Modding API, see README)
# -GamePath <dir>   the Hollow Knight folder, when Steam's library does not find it
# -ModdingApi <zip> the Modding API zip, installed into the oracle copy only
# -AssetDir <dir>   take the release files from this folder instead of downloading them
param(
  [switch]$Game,
  [string]$GamePath = "",
  [string]$ModdingApi = "",
  [string]$AssetDir = ""
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"          # Invoke-WebRequest is 10x slower with the progress bar
$Repo = $PSScriptRoot

# ---- Python
if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
  throw ("uv is not installed. Install it with:`n" +
         "  powershell -ExecutionPolicy Bypass -c `"irm https://astral.sh/uv/install.ps1 | iex`"`n" +
         "then open a new terminal and rerun setup.ps1.")
}
uv sync --project $Repo --quiet
if ($LASTEXITCODE) { throw "uv sync failed" }
"[python] .venv ready"

# ---- release files (release.json: written by tools/package_release.py for this commit)
$manifest = Get-Content (Join-Path $Repo "release.json") -Raw | ConvertFrom-Json
$groups = @("sim", "models") + $(if ($Game) { @("game") } else { @() })
$downloads = Join-Path $Repo "dist\downloads"
New-Item -ItemType Directory -Force $downloads | Out-Null
foreach ($a in $manifest.assets) {
  if ($a.group -notin $groups) { continue }
  $dest = Join-Path $Repo $a.dest
  $marker = Join-Path $dest ".$($a.file).sha256"
  if ((Test-Path $marker) -and (Get-Content $marker -Raw).Trim() -eq $a.sha256) { "[$($a.group)] $($a.file) up to date"; continue }
  $zip = Join-Path $downloads $a.file
  if ($AssetDir) { Copy-Item (Join-Path $AssetDir $a.file) $zip -Force }
  elseif (-not ((Test-Path $zip) -and (Get-FileHash $zip -Algorithm SHA256).Hash -eq $a.sha256)) {
    # gh downloads with your GitHub login, which a private repo's release needs
    if (Get-Command gh -ErrorAction SilentlyContinue) {
      gh release download $manifest.tag -R $manifest.repo -p $a.file -D $downloads --clobber
      if ($LASTEXITCODE) { throw "gh release download failed (gh auth login, and access to $($manifest.repo)?)" }
    } else {
      Invoke-WebRequest "https://github.com/$($manifest.repo)/releases/download/$($manifest.tag)/$($a.file)" -OutFile $zip
    }
  }
  $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
  if ($hash -ne $a.sha256) { throw "$($a.file): sha256 $hash, release.json says $($a.sha256)" }
  New-Item -ItemType Directory -Force $dest | Out-Null
  Expand-Archive $zip -DestinationPath $dest -Force
  Set-Content $marker $a.sha256
  "[$($a.group)] $($a.file) -> $($a.dest)"
}

# ---- the game
if ($Game) {
  $install = @{}
  if ($GamePath) { $install.Game = $GamePath }
  if ($ModdingApi) { $install.ModdingApi = $ModdingApi }
  & (Join-Path $Repo "tools\make_oracle_install.ps1") @install
}

# ---- check everything, with a short training run
& (Join-Path $Repo ".venv\Scripts\python.exe") (Join-Path $Repo "tools\doctor.py") --smoke
