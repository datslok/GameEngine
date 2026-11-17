# Runs the tests in a window of their own, so the progress bar (tests/test_progress.h) shows on the screen while
# someone else runs them (an AI's shell has no visible console): builds tests.exe, runs it in a new console window
# that closes when it ends, prints what it wrote and exits with its code.
#   powershell -File tools/visible_tests.ps1 [-Build release|debug] [-Arguments --times]
param(
    [ValidateSet("release", "debug")] [string]$Build = "release",
    [string]$Arguments = ""
)

$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:PATH
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$folder = if ($Build -eq "release") { "build/release" } else { "build" }

make "BUILD=$Build" "$folder/tests.exe" shaders
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

# The window runs the tests with their output going to a log; the bar is drawn on the window itself.
$log = Join-Path $env:TEMP "game_engine_tests.log"
$exe = Join-Path $root "$folder/tests.exe"
$command = "`$host.UI.RawUI.WindowTitle = 'Tests'; Set-Location '$root'; & '$exe' $Arguments *> '$log'; exit `$LASTEXITCODE"
$window = Start-Process powershell -ArgumentList "-NoProfile", "-Command", $command -PassThru -Wait
Get-Content $log
exit $window.ExitCode
