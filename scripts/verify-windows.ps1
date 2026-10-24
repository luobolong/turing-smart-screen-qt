param([string]$Binary,[switch]$Stress,[switch]$CleanEnvironment,[int]$Seconds=60,[string]$Name='native')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
if(!$Binary){$Binary=Join-Path $taskRoot 'build/windows/Release/turing-screen-qt.exe'}
$taskQt='C:/Users/Administrator/Documents/Workspace/personal/audiomonitor/qt/6.8.0/msvc2022_64'
Push-Location $taskRoot
try {
    if($CleanEnvironment){
        $env:PATH="$env:SystemRoot/System32;$env:SystemRoot"
        Remove-Item Env:QT_PLUGIN_PATH,Env:QT_QPA_PLATFORM,Env:TURING_QT_ASSETS -ErrorAction SilentlyContinue
    }else{$env:PATH="$taskQt/bin;$env:PATH"}
    $taskOutput="artifacts/windows-$Name"
    New-Item -ItemType Directory -Path $taskOutput,"$taskOutput/data" -Force | Out-Null
    @{language='chinese';interval_ms=1000;minimize_to_tray=$true} | ConvertTo-Json | Set-Content -Encoding Ascii "$taskOutput/data/config.json"
    $taskMode=if($Stress){'--stress-test'}else{'--smoke-test'}
    Remove-Item -LiteralPath "$taskOutput/smoke-ok.txt" -ErrorAction SilentlyContinue
    $taskProcess=Start-Process -FilePath $Binary -ArgumentList $taskMode,$taskOutput,'--data-dir',"$taskOutput/data",'--test-seconds',$Seconds -WindowStyle Hidden -PassThru -RedirectStandardError "$taskOutput/stderr.txt"
    $taskHandle=$taskProcess.Handle
    if(!$taskProcess.WaitForExit(($Seconds+30)*1000)){Stop-Process -Id $taskProcess.Id;throw 'Qt test timed out'}
    $taskProcess.WaitForExit();$taskProcess.Refresh()
    if($taskProcess.ExitCode -ne 0 -or !(Test-Path "$taskOutput/smoke-ok.txt")){throw "Qt test failed ($($taskProcess.ExitCode))"}
    $taskReport=Get-Content "$taskOutput/resources.json" -Raw | ConvertFrom-Json
    [pscustomobject]@{mode=$taskMode;resident_MB=[math]::Round($taskReport.peak_resident_bytes/1000000,2);private_MB=[math]::Round($taskReport.peak_private_bytes/1000000,2);idle_CPU_total=[math]::Round($taskReport.idle_cpu_percent_total,3);switches=$taskReport.theme_switches;edits=$taskReport.edits}
    if(!$taskReport.passed){throw '100 MB resident memory target exceeded'}
    $taskReference=Join-Path $taskRoot 'assets/official/config/微星.data'
    $taskRender=Start-Process -FilePath $Binary -ArgumentList '--render-theme',$taskReference,'--output',"$taskOutput/msi.png",'--snapshot','tests/fixtures/official-editor.json','--editor-preview' -WindowStyle Hidden -PassThru -Wait
    if(!(Test-Path "$taskOutput/msi.png")){throw 'Reference theme render failed'}
} finally {Pop-Location}
