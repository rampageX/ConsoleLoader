# ConsoleLoader

Lightweight native Win32 GUI wrapper for Windows console applications and batch scripts. Runs the command in the background, displays stdout/stderr, and highlights common Windows command status messages.

![Screenshot](ConsoleLoader.png)

## Run

```text
ConsoleLoader.exe "D:\Scripts\Start.cmd"
ConsoleLoader.exe "D:\Scripts\Start.cmd" 430x220
ConsoleLoader.exe "C:\Windows\System32\ping.exe" 8.8.8.8 -t 640x360
```

- Optional **last** parameter `WIDTHxHEIGHT`: outer window size in pixels, from 320x160 through 8192x8192.
- Without the size parameter, the window uses the original 1280x720 default.
- Resize a window by dragging its borders, right-click the **title bar**, select **复制窗口尺寸: WIDTHxHEIGHT**, then paste that dimension into the shortcut target.
- Closing ConsoleLoader stops its directly launched process. Applications launched independently by a script (for example `start "" "C:\Program Files (x86)\Proxifier\Proxifier.exe"`) are not killed.
- stdout/stderr output is decoded as UTF-8 when valid, otherwise with the system OEM code page. Windows status lines and numerical latency values are colored.
- The scroll bar is custom-painted to match Windows light/dark mode.

## Build (MSYS2 UCRT64)

```sh
windres ConsoleLoader.rc -O coff -o ConsoleLoader_res.o
g++ -std=c++17 -O2 -pipe -municode -mwindows ConsoleLoader.cpp ConsoleLoader_res.o -o ConsoleLoader.exe -static-libstdc++ -static-libgcc -lole32 -luuid -lcomctl32 -ldwmapi -lgdi32 -lshell32
```

Windows x64 builds run in GitHub Actions on `master` and the test branch. Test executables are available as Actions artifacts; official versioned binaries are published in [Releases](https://github.com/rampageX/ConsoleLoader/releases).

## Compatibility

Use `"path\to\script.cmd" WIDTHxHEIGHT` when launching a batch script. Exact behavior of command-line special characters in batch arguments follows `cmd.exe /D /S /C` rules.
