# Build fxlab with the MSVC compiler from Visual Studio 2022 (no CMake needed).
# Usage: .\build.ps1      -> tools\fxlab\out\fxlab.exe
$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
$vsdev = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $vsdev)) { throw "VsDevCmd.bat not found: $vsdev" }
New-Item -ItemType Directory -Force "$here\out" | Out-Null

$fx = Join-Path $here "..\..\firmware\main\effects"
$sources = @("$here\fxlab.c") + (Get-ChildItem "$fx\*.c" | ForEach-Object { $_.FullName })
$srcArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '

# Run cl inside the VS developer environment (x64), warnings as in a strict C build.
$cmd = "`"$vsdev`" -arch=x64 -no_logo && cl /nologo /O2 /W3 /std:c11 /D_CRT_SECURE_NO_WARNINGS /I`"$fx`" /Fo`"$here\out\\`" /Fe`"$here\out\fxlab.exe`" $srcArgs"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "fxlab build failed" }
Write-Output "built $here\out\fxlab.exe"
