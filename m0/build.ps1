# Build / flash one of the M0 examples with ESP-IDF v5.5.5 and the rev3_x silicon profile.
# Usage:  .\build.ps1 09_video_lcd_display            (build only)
#         .\build.ps1 09_video_lcd_display COM7       (build + flash)
#         .\build.ps1 09_video_lcd_display COM7 rev1_3
param(
    [Parameter(Mandatory = $true)] [string] $Example,
    [string] $Port = "",
    [string] $Profile = "rev3_x"
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

$proj = Join-Path $PSScriptRoot $Example
if (-not (Test-Path $proj)) { throw "Example folder not found: $proj" }
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
