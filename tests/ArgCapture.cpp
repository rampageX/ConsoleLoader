#include <windows.h>
#include <string>

// Record each forwarded argument as a UTF-16 line, including spaces / Unicode.
// argv[1] is the output file path; remaining arguments are the test payload.
int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return 2;
    HANDLE file = CreateFileW(argv[1], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;

    DWORD written = 0;
    const wchar_t bom = 0xFEFF;
    if (!WriteFile(file, &bom, sizeof(bom), &written, nullptr))
    {
        CloseHandle(file);
        return 4;
    }
    for (int i = 2; i < argc; ++i)
    {
        std::wstring line = std::wstring(argv[i]) + L"\r\n";
        const DWORD bytes = static_cast<DWORD>(line.size() * sizeof(wchar_t));
        if (!WriteFile(file, line.data(), bytes, &written, nullptr) || written != bytes)
        {
            CloseHandle(file);
            return 5;
        }
    }
    CloseHandle(file);
    return 0;
}
