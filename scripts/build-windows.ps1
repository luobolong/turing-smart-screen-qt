param([string]$QtRoot='C:/Users/Administrator/Documents/Workspace/personal/audiomonitor/qt/6.8.0/msvc2022_64')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
Push-Location $taskRoot
try {
    cmake -S . -B build/windows -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot" -DBUILD_TESTING=ON
    if($LASTEXITCODE -ne 0){throw 'Configure failed'}
    cmake --build build/windows --config Release --parallel 4
    if($LASTEXITCODE -ne 0){throw 'Build failed'}
    $env:PATH="$QtRoot/bin;$env:PATH"
    ctest --test-dir build/windows -C Release --output-on-failure
    if($LASTEXITCODE -ne 0){throw 'Tests failed'}
} finally {Pop-Location}
