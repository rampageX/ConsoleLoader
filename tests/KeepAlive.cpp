#include <windows.h>
#include <cstdio>

// Test-only stand-in for a GUI application independently launched by a CMD script.
// Do not include this executable in the published ConsoleLoader release.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    FILE* out = nullptr;
    fopen_s(&out, "cl-test-child.pid", "w");
    if (out)
    {
        fprintf(out, "%lu\n", static_cast<unsigned long>(GetCurrentProcessId()));
        fclose(out);
    }
    Sleep(45000);
    return 0;
}
