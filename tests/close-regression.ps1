$ErrorActionPreference = 'Stop'

$root = (Get-Location).Path
$script = Join-Path $root 'cl-test-close.cmd'
$pidFile = Join-Path $root 'cl-test-child.pid'
$launcher = Join-Path $root 'ConsoleLoader.exe'
$childExe = Join-Path $root 'TestTray.exe'
Remove-Item -Path $pidFile -ErrorAction SilentlyContinue

# /B deliberately keeps output handles in the child: the hardest shutdown
# case, which reproduces an inherited writer preventing ReadFile from ending.
@"
@echo off
start "" /B "$childExe"
ping -n 35 127.0.0.1 >nul
"@ | Set-Content -Path $script -Encoding ASCII

$env:CONSOLELOADER_CI_AUTOCLOSE = '1'
try {
    $launcherProc = Start-Process -FilePath $launcher -ArgumentList "`"$script`" 430x220" -WorkingDirectory $root -PassThru
}
finally {
    Remove-Item Env:CONSOLELOADER_CI_AUTOCLOSE -ErrorAction SilentlyContinue
}
$childPid = $null
try {
    for ($i = 0; $i -lt 80; $i++) {
        if (Test-Path $pidFile) {
            $data = (Get-Content $pidFile -Raw).Trim()
            if ($data -match '^\d+$') {
                $childPid = [int]$data
                break
            }
        }
        if ($launcherProc.HasExited) { throw 'ConsoleLoader exited unexpectedly before close.' }
        Start-Sleep -Milliseconds 200
    }
    if (!$childPid) { throw 'Detached GUI child did not start.' }

    # The app posts WM_CLOSE to itself after 2 seconds in CI-only mode.
    # No interactive desktop or HWND enumeration is needed on GitHub's runner.
    if (-not $launcherProc.WaitForExit(10000)) {
        throw 'REGRESSION: ConsoleLoader.exe remains after closing its window.'
    }
    if (-not (Get-Process -Id $childPid -ErrorAction SilentlyContinue)) {
        throw 'REGRESSION: independent GUI child was killed with ConsoleLoader.'
    }
    Write-Host "PASS: ConsoleLoader pid $($launcherProc.Id) exited; detached child pid $childPid survived."
}
finally {
    if ($childPid) {
        Stop-Process -Id $childPid -Force -ErrorAction SilentlyContinue
    }
    if (-not $launcherProc.HasExited) {
        Stop-Process -Id $launcherProc.Id -Force -ErrorAction SilentlyContinue
    }
    $launcherProc.Dispose()
    Remove-Item -Path $script,$pidFile -ErrorAction SilentlyContinue
}
