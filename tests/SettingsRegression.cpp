// Reuse the production code to test actual registry and RichEdit behavior.
// No network, user-specific files, or interactive desktop automation required.
#define wWinMain ConsoleLoaderEntryNotUsedByTest
#include "../ConsoleLoader.cpp"
#undef wWinMain

#include <stdexcept>
#include <cstdio>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int wmain()
{
    const wchar_t* originalKey = g_settingsKey;
    g_settingsKey = L"Software\\rampageX\\ConsoleLoader\\CI_Regression";
    RegDeleteTreeW(HKEY_CURRENT_USER, g_settingsKey);
    try
    {
        LoadSettings();
        Require(!g_autoCopy && !g_grayAA && g_fontName == L"Consolas" && g_fontSize == 9,
                "default settings mismatch");

        g_autoCopy = true;
        g_grayAA = true;
        g_fontName = L"Consolas";
        g_fontSize = 14;
        g_fontWeight = FW_BOLD;
        g_fontItalic = 1;
        Require(SaveSettings(), "cannot save HKCU settings");

        g_autoCopy = false;
        g_grayAA = false;
        g_fontName = L"Wrong";
        g_fontSize = 7;
        g_fontWeight = FW_NORMAL;
        g_fontItalic = 0;
        LoadSettings();
        Require(g_autoCopy && g_grayAA && g_fontName == L"Consolas" && g_fontSize == 14 &&
                g_fontWeight == FW_BOLD && g_fontItalic == 1,
                "HKCU settings round-trip failed");
        puts("PASS: registry defaults and persistent settings");

        HKEY key = nullptr;
        Require(RegOpenKeyExW(HKEY_CURRENT_USER, g_settingsKey, 0,
                              KEY_SET_VALUE, &key) == ERROR_SUCCESS,
                "cannot open test settings key");
        const DWORD invalidSize = 1000;
        const LONG status = RegSetValueExW(key, L"FontSize", 0, REG_DWORD,
                                         reinterpret_cast<const BYTE*>(&invalidSize),
                                         sizeof(invalidSize));
        RegCloseKey(key);
        Require(status == ERROR_SUCCESS, "cannot inject invalid font size");
        LoadSettings();
        Require(g_fontSize == 9, "invalid saved font size was not rejected");
        puts("PASS: invalid saved font size falls back to default");

        HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");
        Require(richEdit != nullptr, "Msftedit.dll not available");
        g_hWnd = CreateWindowExW(0, L"STATIC", L"Regression", WS_OVERLAPPEDWINDOW,
                                 0, 0, 500, 200, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        Require(g_hWnd != nullptr, "test parent window creation failed");
        g_hEdit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                  WS_CHILD | ES_MULTILINE | ES_READONLY,
                                  0, 0, 480, 180, g_hWnd, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        Require(g_hEdit != nullptr, "test RichEdit creation failed");

        SendMessageW(g_hEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Alpha 中文 Beta"));
        CHARRANGE selection{6, 8};
        SendMessageW(g_hEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        Require(SelectedPlainText(g_hEdit) == L"中文",
                "selected Unicode text does not match");
        puts("PASS: Unicode selection extraction");

        selection = CHARRANGE{0, 5};
        SendMessageW(g_hEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        CHARFORMATW color{};
        color.cbSize = sizeof(color);
        color.dwMask = CFM_COLOR;
        color.crTextColor = RGB(16, 32, 64);
        Require(SendMessageW(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION,
                             reinterpret_cast<LPARAM>(&color)) != 0,
                "cannot apply test color");

        g_grayAA = false;
        g_fontName = L"Consolas";
        g_fontSize = 16;
        g_fontWeight = FW_NORMAL;
        g_fontItalic = 0;
        ApplyDisplayFont();
        selection = CHARRANGE{1, 2};
        SendMessageW(g_hEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        CHARFORMATW actual{};
        actual.cbSize = sizeof(actual);
        SendMessageW(g_hEdit, EM_GETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&actual));
        Require((actual.dwMask & CFM_COLOR) != 0 &&
                actual.crTextColor == RGB(16, 32, 64),
                "font application erased a highlighted span color");
        Require((actual.dwMask & CFM_SIZE) != 0 && actual.yHeight == 320,
                "font size was not applied to existing text");
        Require((actual.dwMask & CFM_FACE) != 0 &&
                wcscmp(actual.szFaceName, L"Consolas") == 0,
                "font family was not applied to existing text");
        puts("PASS: font dialog settings apply without losing text highlighting");

        LOGFONTW fontInfo{};
        Require(g_displayFont &&
                GetObjectW(g_displayFont, sizeof(fontInfo), &fontInfo) == sizeof(fontInfo) &&
                fontInfo.lfQuality == DEFAULT_QUALITY,
                "default font has an unexpected smoothing quality");

        g_grayAA = true;
        ApplyDisplayFont();
        Require(g_displayFont &&
                GetObjectW(g_displayFont, sizeof(fontInfo), &fontInfo) == sizeof(fontInfo) &&
                fontInfo.lfQuality == ANTIALIASED_QUALITY,
                "experimental grayscale font quality was not set");
        SendMessageW(g_hEdit, EM_GETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&actual));
        Require((actual.dwMask & CFM_COLOR) != 0 &&
                actual.crTextColor == RGB(16, 32, 64) &&
                (actual.dwMask & CFM_SIZE) != 0 && actual.yHeight == 320,
                "experimental font quality toggle changed text colors or point size");
        Require(SaveSettings(), "grayscale setting could not be saved");
        g_grayAA = false;
        LoadSettings();
        Require(g_grayAA, "grayscale setting was not restored from HKCU");
        g_grayAA = false;
        ApplyDisplayFont();
        Require(g_displayFont &&
                GetObjectW(g_displayFont, sizeof(fontInfo), &fontInfo) == sizeof(fontInfo) &&
                fontInfo.lfQuality == DEFAULT_QUALITY,
                "switching back did not restore Windows default smoothing");
        puts("PASS: reversible font-quality hint, registry persistence and formatting");

        if (g_displayFont) { DeleteObject(g_displayFont); g_displayFont = nullptr; }
        DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
        g_hEdit = nullptr;
        FreeLibrary(richEdit);
        RegDeleteTreeW(HKEY_CURRENT_USER, g_settingsKey);
        g_settingsKey = originalKey;
        return 0;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        if (g_displayFont) { DeleteObject(g_displayFont); g_displayFont = nullptr; }
        if (g_hWnd) DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
        g_hEdit = nullptr;
        RegDeleteTreeW(HKEY_CURRENT_USER, g_settingsKey);
        g_settingsKey = originalKey;
        return 1;
    }
}
