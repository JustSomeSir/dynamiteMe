# Builds dynamite.exe from the QA package and installs it into BOTH test copies.
# Run once after a new package lands in E:\coop-mgsv\build:
#   powershell -ExecutionPolicy Bypass -File E:\coop-mgsv\qa\prepare.ps1 -Package dynamiteMe-qa-<hash>.zip
# Touches only E:\coop-mgsv\build and the two test copies (game, game2). Never the Steam install.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Root = "E:\coop-mgsv"
)
$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 turns a native program's stderr into a terminating error under "Stop"
# (go and dynamite.exe log to stderr). Run them with "Continue" and judge by the exit code only.
function Invoke-Native([string]$What, [string]$Log, [scriptblock]$Cmd) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Cmd *> $Log } finally { $ErrorActionPreference = $old }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE), see $Log" }
}

$zip = Join-Path "$Root\build" $Package
if (-not (Test-Path $zip)) { throw "package not found: $zip" }
$src = "$Root\build\dynamiteMe-qa"
if (Test-Path $src) { Remove-Item -Recurse -Force $src }
Expand-Archive -Path $zip -DestinationPath "$Root\build"

Push-Location $src
try {
    $env:CGO_ENABLED = "0"
    Write-Host "== go generate"
    Invoke-Native "go generate" "$Root\build\qa-gen.log" { go generate . }
    Write-Host "== go build"
    Invoke-Native "go build" "$Root\build\qa-build.log" { go build -trimpath -ldflags="-w -s" -o dynamite.exe . }
} finally { Pop-Location }

foreach ($g in "game", "game2") {
    $dir = "$Root\$g"
    if (Get-Process -Name mgsvtpp -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir\*" }) {
        throw "mgsvtpp.exe from $dir is running; close it first"
    }
    if (Get-Process -Name dynamite -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir\*" }) {
        throw "dynamite.exe (master server / launcher) from $dir is running; close its window first"
    }
    Copy-Item "$src\dynamite.exe" "$dir\dynamite.exe" -Force
    Write-Host "== install into $dir"
    Push-Location $dir
    try {
        Invoke-Native "install into $dir" "$Root\build\qa-install-$g.log" { & "$dir\dynamite.exe" -installNow -forceInstall -installAndExit }
    } finally { Pop-Location }
    $h = (Get-FileHash "$dir\dinput8.dll" -Algorithm SHA256).Hash.ToLower()
    Write-Host "   dinput8.dll $h"
}
Write-Host "== done. Next: cd $Root\qa; python run_qa.py --twins"
