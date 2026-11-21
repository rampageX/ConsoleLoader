$ErrorActionPreference = 'Stop'

$root = (Get-Location).Path
$launcher = Join-Path $root 'ConsoleLoader.exe'
$capture = Join-Path $root 'ArgCapture.exe'
$video = Join-Path $root 'Sample 影片 & spaces.mp4'
$secondVideo = Join-Path $root 'Another sample.mov'

function Test-Arguments {
    param(
        [string]$Name,
        [string[]]$Arguments,
        [string[]]$Expected
    )

    $outFile = Join-Path $root ("cl-args-" + $Name + ".txt")
    Remove-Item -LiteralPath $outFile -ErrorAction SilentlyContinue

    $resolved = @(
        foreach ($arg in $Arguments) {
            if ($arg -eq '{OUT}') { $outFile } else { $arg }
        }
    )
    # Quote each argument as Explorer does when it appends selected file paths.
    $quoted = @(
        foreach ($arg in (@($capture) + $resolved)) {
            '"' + $arg.Replace('"', '\"') + '"'
        }
    )
    $argLine = $quoted -join ' '

    $env:CONSOLELOADER_CI_AUTOCLOSE = '1'
    $process = $null
    try {
        $process = Start-Process -FilePath $launcher -ArgumentList $argLine -WorkingDirectory $root -PassThru
    }
    finally {
        Remove-Item Env:CONSOLELOADER_CI_AUTOCLOSE -ErrorAction SilentlyContinue
    }

    try {
        for ($i = 0; $i -lt 60; $i++) {
            if ((Test-Path -LiteralPath $outFile) -and ((Get-Item -LiteralPath $outFile).Length -gt 2)) {
                break
            }
            if ($process.HasExited) { break }
            Start-Sleep -Milliseconds 100
        }

        if (!(Test-Path -LiteralPath $outFile)) {
            throw "${Name}: child did not create output file (misparsed arguments)."
        }
        $actual = @([System.IO.File]::ReadAllLines($outFile, [System.Text.Encoding]::Unicode))
        if ($actual.Count -ne $Expected.Count) {
            throw "${Name}: argument count differs: got $($actual.Count), expected $($Expected.Count). Actual: $($actual -join ' | ')"
        }
        for ($i = 0; $i -lt $Expected.Count; $i++) {
            if ($actual[$i] -cne $Expected[$i]) {
                throw "${Name}: argv[$i] expected '$($Expected[$i])', got '$($actual[$i])'."
            }
        }
        if (!$process.WaitForExit(10000)) {
            throw "${Name}: ConsoleLoader did not exit after CI close timer."
        }
        Write-Host "PASS: $Name -> $($actual -join ' | ')"
    }
    finally {
        if ($process -and !$process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        if ($process) { $process.Dispose() }
        Remove-Item -LiteralPath $outFile -ErrorAction SilentlyContinue
    }
}

# Windows Send To: shortcut arguments precede the appended selected files.
Test-Arguments 'sendto-one' @('650x400', '{OUT}', $video) @($video)
Test-Arguments 'sendto-multiple' @('650x400', '{OUT}', $video, $secondVideo) @($video, $secondVideo)

# Preserve the existing final-argument syntax and the default behavior.
Test-Arguments 'last-position' @('{OUT}', $video, '650x400') @($video)
Test-Arguments 'no-size' @('{OUT}', $video) @($video)

# Invalid dimensions are not silently consumed.
Test-Arguments 'invalid-size' @('{OUT}', '300x150', $video) @('300x150', $video)
