param([string]$QtRoot='C:/Users/Administrator/Documents/Workspace/personal/audiomonitor/qt/6.8.0/msvc2022_64')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
Push-Location $taskRoot
try {
    $taskDestination=Join-Path $taskRoot 'dist/turing-screen-qt-0.2.0-windows-x64'
    if(Test-Path -LiteralPath $taskDestination){throw 'Release directory already exists; choose or archive the existing release first.'}
    New-Item -ItemType Directory -Path $taskDestination,"$taskDestination/licenses" -Force | Out-Null
    Copy-Item -LiteralPath 'build/windows/Release/turing-screen-qt.exe' -Destination $taskDestination
    Copy-Item -LiteralPath assets,docs -Destination $taskDestination -Recurse
    Copy-Item -LiteralPath README.md,LICENSE,THIRD_PARTY.md -Destination $taskDestination
    Copy-Item -LiteralPath LICENSE -Destination "$taskDestination/licenses/Qt-GPL-3.0.txt"
    $env:PATH="$QtRoot/bin;$env:PATH"
    & "$QtRoot/bin/windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler --no-quick-import --no-compiler-runtime --no-plugins "$taskDestination/turing-screen-qt.exe"
    if($LASTEXITCODE -ne 0){throw 'Qt deployment failed'}
    New-Item -ItemType Directory -Path "$taskDestination/plugins/platforms","$taskDestination/plugins/imageformats" -Force | Out-Null
    Copy-Item -LiteralPath "$QtRoot/plugins/platforms/qwindows.dll","$QtRoot/plugins/platforms/qoffscreen.dll" -Destination "$taskDestination/plugins/platforms"
    Copy-Item -LiteralPath "$QtRoot/plugins/imageformats/qjpeg.dll" -Destination "$taskDestination/plugins/imageformats"
    $taskCrt=Get-ChildItem -LiteralPath 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC' -Directory | Where-Object {$_.Name -match '^14\.'} | Sort-Object Name -Descending | Select-Object -First 1
    Copy-Item -Path (Join-Path $taskCrt.FullName 'x64/Microsoft.VC143.CRT/*.dll') -Destination $taskDestination
    "[Paths]`nPrefix=.`nPlugins=plugins`n" | Set-Content -Encoding Ascii "$taskDestination/qt.conf"
    Compress-Archive -LiteralPath $taskDestination -DestinationPath "$taskDestination.zip" -CompressionLevel Optimal
    Get-Item -LiteralPath "$taskDestination.zip" | Select-Object FullName,Length
} finally {Pop-Location}
