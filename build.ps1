# Builds with CMake and Ninja from a Visual Studio install, then runs the tests.
#   .\build.ps1            debug
#   .\build.ps1 -Release   optimised
#   .\build.ps1 -NoTests   skip running the tests
param([switch]$Release, [switch]$NoTests)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio not found (vswhere.exe is missing).' }

# Any install with the C++ tools and its bundled CMake: not every one has both.
$vs = $null
foreach ($path in & $vswhere -all -products * -property installationPath) {
    $cmake = Join-Path $path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $vcvars = Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'
    if ((Test-Path $cmake) -and (Test-Path $vcvars)) { $vs = $path; break }
}
if (-not $vs) { throw 'No Visual Studio install has both the C++ tools and the CMake component.' }

$config = if ($Release) { 'Release' } else { 'Debug' }
$build = "build\$($config.ToLower())"
$bin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$steps = @(
    "call `"$vcvars`" >nul 2>nul"
    "set `"PATH=$bin\CMake\bin;$bin\Ninja;%PATH%`""
    "cmake -S . -B $build -G Ninja -DCMAKE_BUILD_TYPE=$config || exit /b 1"
    "cmake --build $build || exit /b 1"
)
if (-not $NoTests) { $steps += "ctest --test-dir $build --output-on-failure || exit /b 1" }

$script = Join-Path $env:TEMP "tp_build_$PID.bat"
Set-Content -Path $script -Value ('@echo off', ($steps -join "`r`n")) -Encoding ascii
try {
    & cmd /c $script
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
} finally {
    Remove-Item $script -ErrorAction SilentlyContinue
}
