[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$projectDir = $PSScriptRoot
$buildDir = Join-Path $projectDir 'build'

foreach ($tool in @('cmake', 'ninja', 'g++', 'ctest')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required build tool is not on PATH: $tool"
    }
}

cmake -S $projectDir -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    '-DCMAKE_CXX_COMPILER=g++'
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }

if (-not $SkipTests) {
    ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}

Write-Host "Built: $(Join-Path $buildDir 'stereo_surround.exe')"
