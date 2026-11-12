# Copies a few files out of the test copy's archives so their structure can be studied (read-only on the game copy).
#   powershell -ExecutionPolicy Bypass -File E:\coop-mgsv\qa\extract_gamefiles.ps1
# Output: E:\coop-mgsv\build\gamefiles\  (game data: stays on this PC and out of every repository)
param(
    [string]$Root = "E:\coop-mgsv",
    [string[]]$Paths = @(
        "/Assets/tpp/pack/buddy/horse/buddy_horse2_common.fpkd",
        "/Assets/tpp/pack/buddy/horse/buddy_horse2_common.fpk",
        "/Assets/tpp/pack/buddy/horse/buddy_horse2_00.fpkd"
    )
)
$src = "$Root\build\dynamiteMe-qa"
if (-not (Test-Path "$src\go.mod")) { throw "source not found: $src (run prepare.ps1 once)" }
if (-not (Test-Path "$src\tools\gamefiles\main.go")) { throw "tools\gamefiles\main.go is missing in $src" }
$out = "$Root\build\gamefiles"
Push-Location $src
try {
    $env:CGO_ENABLED = "0"
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    & go run ./tools/gamefiles -game "$Root\game" -out $out @Paths 2>&1 | Tee-Object "$Root\build\gamefiles.log"
    $ErrorActionPreference = $old
} finally { Pop-Location }
Write-Host "== log: $Root\build\gamefiles.log"
