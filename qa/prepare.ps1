# Builds dynamite.exe from the QA package and installs it into BOTH test copies.
# Run once after a new package lands in E:\coop-mgsv\build:
#   powershell -ExecutionPolicy Bypass -File E:\coop-mgsv\qa\prepare.ps1 -Package dynamiteMe-qa-<hash>.zip
# Touches only E:\coop-mgsv\build and the two test copies (game, game2). Never the Steam install.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Root = "E:\coop-mgsv"
)
$ErrorActionPreference = "Stop"

$zip = Join-Path "$Root\build" $Package
if (-not (Test-Path $zip)) { throw "package not found: $zip" }
$src = "$Root\build\dynamiteMe-qa"
if (Test-Path $src) { Remove-Item -Recurse -Force $src }
Expand-Archive -Path $zip -DestinationPath "$Root\build"

Push-Location $src
try {
    $env:CGO_ENABLED = "0"
    Write-Host "== go generate"
    go generate . *> "$Root\build\qa-gen.log"
    if ($LASTEXITCODE -ne 0) { throw "go generate failed, see $Root\build\qa-gen.log" }
    Write-Host "== go build"
    go build -trimpath -ldflags="-w -s" -o dynamite.exe . *> "$Root\build\qa-build.log"
    if ($LASTEXITCODE -ne 0) { throw "go build failed, see $Root\build\qa-build.log" }
} finally { Pop-Location }

foreach ($g in "game", "game2") {
    $dir = "$Root\$g"
    if (Get-Process -Name mgsvtpp -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir\*" }) {
        throw "mgsvtpp.exe from $dir is running; close it first"
    }
    Copy-Item "$src\dynamite.exe" "$dir\dynamite.exe" -Force
    Write-Host "== install into $dir"
    Push-Location $dir
    try {
        & "$dir\dynamite.exe" -installNow -forceInstall -installAndExit *> "$Root\build\qa-install-$g.log"
        if ($LASTEXITCODE -ne 0) { throw "install into $dir failed (exit $LASTEXITCODE), see $Root\build\qa-install-$g.log" }
    } finally { Pop-Location }
    $h = (Get-FileHash "$dir\dinput8.dll" -Algorithm SHA256).Hash.ToLower()
    Write-Host "   dinput8.dll $h"
}
Write-Host "== done. Next: cd $Root\qa; python run_qa.py --twins"
