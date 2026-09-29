# Measures StudyBoard's start-up on Windows (Phase 9 baseline, docs/PERFORMANCE.md):
# the time from process start until the main window exists and the GUI thread is idle
# (WaitForInputIdle), and the working set a few seconds after that. Runs the executable
# several times against a scratch workspace; the first run is the "cold" one (file cache).
#
#   pwsh tools/measure_startup.ps1 -Exe build/release/app/studyapp.exe [-Runs 5]
#                                  [-Workspace <dir>]
#
# Uses a private QSettings location by overriding APPDATA for the child process, so the
# user's settings and recent-workspace list are not touched.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [int]$Runs = 5,
    [string]$Workspace = ''
)
$ErrorActionPreference = 'Stop'
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("studyboard-startup-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force $scratch | Out-Null
if (-not $Workspace) { $Workspace = Join-Path $scratch 'Startup.studyws' }
$results = @()
try {
    for ($i = 1; $i -le $Runs; ++$i) {
        $info = New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName = (Resolve-Path $Exe).Path
        $info.Arguments = "--workspace `"$Workspace`""
        $info.UseShellExecute = $false
        $info.EnvironmentVariables['APPDATA'] = $scratch
        $info.EnvironmentVariables['LOCALAPPDATA'] = $scratch
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $process = [System.Diagnostics.Process]::Start($info)
        $null = $process.WaitForInputIdle(30000)
        while ($process.MainWindowHandle -eq 0 -and $watch.ElapsedMilliseconds -lt 30000) {
            Start-Sleep -Milliseconds 5
            $process.Refresh()
        }
        $null = $process.WaitForInputIdle(30000)
        $ready = $watch.ElapsedMilliseconds
        Start-Sleep -Seconds 3
        $process.Refresh()
        $results += [pscustomobject]@{
            Run = $i
            ReadyMs = $ready
            WorkingSetMB = [math]::Round($process.WorkingSet64 / 1MB, 1)
            PrivateMB = [math]::Round($process.PrivateMemorySize64 / 1MB, 1)
        }
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(10000)) { $process.Kill() }
    }
} finally {
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
$results | Format-Table -AutoSize | Out-String
$warm = $results | Select-Object -Skip 1
"cold: {0} ms; warm median: {1} ms" -f $results[0].ReadyMs, (($warm.ReadyMs | Sort-Object)[[int][math]::Floor($warm.Count / 2)])
