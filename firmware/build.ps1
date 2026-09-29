# Build / flash the glitchESP firmware with ESP-IDF v5.5.5 (rev1_3 silicon profile by default).
# Usage:  .\build.ps1                 (build only)
#         .\build.ps1 COM10           (build + flash)
#         .\build.ps1 COM10 rev3_x    (other silicon profile)
param(
    [string] $Port = "",
    [string] $Profile = "rev1_3"
)
# Native tools (git, idf.py) write informational text to stderr; PowerShell 5.1 would
# turn that into terminating errors under "Stop", so rely on exit codes instead.
$ErrorActionPreference = "Continue"
$IdfPath = "$env:USERPROFILE\esp\v5.5.5\esp-idf"
if (-not (Test-Path "$IdfPath\export.ps1")) { throw "ESP-IDF not found at $IdfPath" }

# export.ps1 sets IDF_PATH, PATH and the python env for this process.
# It prints "Activating ESP-IDF" on stderr, which PowerShell 5.1 would turn into
# a terminating error under ErrorActionPreference=Stop, so relax it while sourcing.
. "$IdfPath\export.ps1" 2>$null | Out-Null

$proj = $PSScriptRoot

Set-Location $proj

$build = "build/$Profile"
$args = @("-B", $build,
          "-D", "SDKCONFIG=$proj/$build/sdkconfig",
          "-D", "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.$Profile")

idf.py @args build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if ($Port -ne "") {
    idf.py @args -p $Port flash
    if ($LASTEXITCODE -ne 0) { throw "flash failed" }
}
