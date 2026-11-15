$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CLWin32 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string className, string windowName);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
}
'@

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

$launcherProc = Start-Process -FilePath $launcher -ArgumentList "`"$script`" 430x220" -WorkingDirectory $root -PassThru
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

    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 50; $i++) {
        $hwnd = [CLWin32]::FindWindowW('ConsoleLoader', $null)
        if ($hwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw 'ConsoleLoader window not found.' }
    if (-not [CLWin32]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Unable to send WM_CLOSE.'
    }

    if (-not $launcherProc.WaitForExit(6000)) {
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
