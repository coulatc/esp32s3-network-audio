param(
    [ValidateSet("build", "flash", "monitor")]
    [string]$Action = "build",
    [string]$Port = "COM7",
    [string]$BuildDir,
    [string]$Profile = "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    . $Profile
}
$env:PYTHONUTF8 = "1"
Remove-Item Env:IDF_COMPONENT_LOCAL_STORAGE_URL -ErrorAction SilentlyContinue
Push-Location $PSScriptRoot
try {
    $defaults = "sdkconfig.defaults"
    if ($Configuration -eq "Debug") {
        if (!$BuildDir) { $BuildDir = "build-debug" }
        $defaults += ";sdkconfig.debug.defaults"
    }
    if (!$BuildDir) { $BuildDir = "build" }
    $output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir).Replace('\', '/')
    $configFile = "$output/sdkconfig.$($Configuration.ToLowerInvariant())"
    $idfArgs = @("-B", $output, "-D", "SDKCONFIG=$configFile", "-D", "SDKCONFIG_DEFAULTS=$defaults")
    if ($Action -eq "build") {
        idf.py @idfArgs build
    } else {
        idf.py @idfArgs -p $Port $Action
    }
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}
