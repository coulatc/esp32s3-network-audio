param(
    [ValidateSet("build", "flash", "monitor")]
    [string]$Action = "build",
    [string]$Port = "COM7",
    [string]$BuildDir = "build",
    [string]$Profile = "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
)

$ErrorActionPreference = "Stop"
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    . $Profile
}
$env:PYTHONUTF8 = "1"
Remove-Item Env:IDF_COMPONENT_LOCAL_STORAGE_URL -ErrorAction SilentlyContinue
Push-Location $PSScriptRoot
try {
    if ($Action -eq "build") {
        idf.py -B $BuildDir build
    } else {
        idf.py -B $BuildDir -p $Port $Action
    }
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}
