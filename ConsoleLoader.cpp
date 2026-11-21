
#include <windows.h>
#include <dwmapi.h>
#include <richedit.h>
#include <commdlg.h>
#include <string>
#include <cwctype>
#include <shellapi.h>
#include <algorithm>
#include <vector>
#include <limits>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <atomic>

#pragma comment(lib, "dwmapi.lib")

/* ================= 全局（核心原样） ================= */

HWND   g_hWnd  = nullptr;
HWND   g_hEdit = nullptr;
HWND   g_hScroll = nullptr;
HANDLE g_hRead = nullptr;
HANDLE g_hProc = nullptr;
HANDLE g_hJob  = nullptr;
HANDLE g_hPipeThread = nullptr;
std::atomic<bool> g_shuttingDown{false};

#define WM_APPEND (WM_APP + 1)
#define IDM_COPY_SIZE 0x1F00
#define IDM_AUTO_COPY 0x1F10
#define IDM_CHOOSE_FONT 0x1F20
#define IDM_GRAYSCALE_AA 0x1F30
#define TIMER_COPY_NOTICE 7
#define TIMER_CI_CLOSE 8
#define TIMER_STARTUP_ACTIVATE 9

std::wstring g_baseTitle;
static int g_activationAttempts = 0;

// Per-user settings; no external configuration file and no elevation needed.
static const wchar_t* g_settingsKey = L"Software\\rampageX\\ConsoleLoader";
static bool g_autoCopy = false;
// Experimental: request GDI grayscale antialiasing per font, not system-wide.
static bool g_grayAA = false;
static std::wstring g_fontName = L"Consolas";
static DWORD g_fontSize = 9;  // Points.
static DWORD g_fontWeight = FW_NORMAL;
static DWORD g_fontItalic = 0;
static HFONT g_displayFont = nullptr;

static void LoadSettings()
{
    // Reset to defaults if the key or one of its values is missing/invalid.
    g_autoCopy = false;
    g_grayAA = false;
    g_fontName = L"Consolas";
    g_fontSize = 9;
    g_fontWeight = FW_NORMAL;
    g_fontItalic = 0;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, g_settingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;

    auto readDword = [key](const wchar_t* name, DWORD& value) {
        DWORD type = 0, size = sizeof(DWORD), read = 0;
        if (RegQueryValueExW(key, name, nullptr, &type,
                            reinterpret_cast<LPBYTE>(&read), &size) == ERROR_SUCCESS &&
            type == REG_DWORD && size == sizeof(DWORD))
            value = read;
    };

    DWORD copy = 0;
    readDword(L"AutoCopy", copy);
    g_autoCopy = copy != 0;
    DWORD grayscale = 0;
    readDword(L"GrayscaleAA", grayscale);
    g_grayAA = grayscale != 0;

    wchar_t face[LF_FACESIZE]{};
    DWORD type = 0, size = sizeof(face);
    if (RegQueryValueExW(key, L"FontName", nullptr, &type,
                         reinterpret_cast<LPBYTE>(face), &size) == ERROR_SUCCESS &&
        type == REG_SZ && face[0])
    {
        face[LF_FACESIZE - 1] = L'\0';
        g_fontName = face;
    }

    readDword(L"FontSize", g_fontSize);
    readDword(L"FontWeight", g_fontWeight);
    readDword(L"FontItalic", g_fontItalic);
    if (g_fontSize < 6 || g_fontSize > 96) g_fontSize = 9;
    if (g_fontWeight > 1000) g_fontWeight = FW_NORMAL;
    g_fontItalic = g_fontItalic != 0;
    RegCloseKey(key);
}

static bool SaveSettings()
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, g_settingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    auto writeDword = [key](const wchar_t* name, DWORD value) {
        return RegSetValueExW(key, name, 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
    };
    const bool saved =
        writeDword(L"AutoCopy", g_autoCopy ? 1 : 0) &&
        writeDword(L"GrayscaleAA", g_grayAA ? 1 : 0) &&
        RegSetValueExW(key, L"FontName", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(g_fontName.c_str()),
                       static_cast<DWORD>((g_fontName.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS &&
        writeDword(L"FontSize", g_fontSize) &&
        writeDword(L"FontWeight", g_fontWeight) &&
        writeDword(L"FontItalic", g_fontItalic);
    RegCloseKey(key);
    return saved;
}

// After UAC consent, Windows may leave a newly created window behind Explorer.
// Raise it only during startup; never keep the topmost extended style.
// Windows may deny SetForegroundWindow under foreground lock policy, so use
// a reversible z-order bump as a fallback for visibility.
static void ActivateStartupWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return;

    if (GetForegroundWindow() == hwnd)
    {
        KillTimer(hwnd, TIMER_STARTUP_ACTIVATE);
        return;
    }

    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
                       SWP_NOACTIVATE | SWP_SHOWWINDOW;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    SetForegroundWindow(hwnd);
    if (GetForegroundWindow() == hwnd)
        KillTimer(hwnd, TIMER_STARTUP_ACTIVATE);
}


static bool ParseWindowSize(const std::wstring& input, int& width, int& height)
{
    size_t x = input.find_first_of(L"xX");
    if (x == std::wstring::npos || x == 0 || x + 1 == input.size()) return false;
    if (input.find_first_of(L"xX", x + 1) != std::wstring::npos) return false;
    for (size_t i = 0; i < input.size(); ++i)
        if (i != x && !iswdigit(input[i])) return false;
    wchar_t* end = nullptr;
    unsigned long w = wcstoul(input.c_str(), &end, 10);
    if (end != input.c_str() + x || w < 320 || w > 8192) return false;
    unsigned long h = wcstoul(input.c_str() + x + 1, &end, 10);
    if (*end || h < 160 || h > 8192) return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

static std::wstring QuoteWinArg(const std::wstring& arg)
{
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : arg)
    {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
        else { out.append(slashes, L'\\'); out += c; }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

static bool EndsWithIgnoreCase(const std::wstring& value, const wchar_t* suffix)
{
    size_t n = wcslen(suffix);
    if (value.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (towlower(value[value.size() - n + i]) != towlower(suffix[i])) return false;
    return true;
}

static std::wstring WinError(DWORD error)
{
    LPWSTR details = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&details), 0, nullptr);
    std::wstring text = details ? details : L"未知错误";
    if (details) LocalFree(details);
    return L"错误 " + std::to_wstring(error) + L": " + text;
}

static bool CopyClipboard(HWND owner, const std::wstring& data)
{
    if (!OpenClipboard(owner)) return false;
    if (!EmptyClipboard()) { CloseClipboard(); return false; }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (data.size() + 1) * sizeof(wchar_t));
    if (!mem) { CloseClipboard(); return false; }
    void* dst = GlobalLock(mem);
    if (!dst) { GlobalFree(mem); CloseClipboard(); return false; }
    memcpy(dst, data.c_str(), (data.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem))
    {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

/* ================= 自绘轻量滚动条（明暗主题均不依赖系统皮肤） ================= */

bool IsDarkMode();

static int g_dragOffset = -1;

struct ScrollMetrics
{
    int count = 1;
    int visible = 1;
    int first = 0;
    int maxFirst = 0;
    int thumbTop = 0;
    int thumbHeight = 0;
};

static int EditLineHeight()
{
    if (!g_hEdit) return 16;
    HDC dc = GetDC(g_hEdit);
    if (!dc) return 16;
    HFONT font = g_displayFont ? g_displayFont :
        reinterpret_cast<HFONT>(SendMessageW(g_hEdit, WM_GETFONT, 0, 0));
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    if (previous) SelectObject(dc, previous);
    ReleaseDC(g_hEdit, dc);
    return (std::max)(1L, metrics.tmHeight);
}

static ScrollMetrics GetScrollMetrics(int clientHeight)
{
    ScrollMetrics m;
    if (!g_hEdit) return m;
    RECT rc{};
    GetClientRect(g_hEdit, &rc);
    m.count = (std::max)(1, static_cast<int>(SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0)));
    m.visible = (std::max)(1, static_cast<int>((rc.bottom - rc.top) / EditLineHeight()));
    m.first = (std::max)(0, static_cast<int>(SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0)));
    m.maxFirst = (std::max)(0, m.count - m.visible);
    m.first = (std::min)(m.first, m.maxFirst);
    m.thumbHeight = m.maxFirst == 0 ? clientHeight :
        (std::min)(clientHeight, (std::max)(28, clientHeight * m.visible / m.count));
    m.thumbTop = m.maxFirst == 0 ? 0 :
        (clientHeight - m.thumbHeight) * m.first / m.maxFirst;
    return m;
}

// Keep the document full width when it fits. When it overflows, the
// custom scrollbar occupies only its own 12px area; no unpainted seam remains.
static void UpdateScrollLayout()
{
    if (!g_hWnd || !g_hEdit || !g_hScroll) return;

    RECT rc{};
    GetClientRect(g_hWnd, &rc);
    const int width = (std::max)(0, static_cast<int>(rc.right - rc.left - 2));
    const int height = (std::max)(0, static_cast<int>(rc.bottom - rc.top - 2));
    constexpr int barWidth = 12;

    // First measure with all available width. A narrower editor could
    // introduce soft-wrapped lines that disappear when the bar is hidden.
    MoveWindow(g_hEdit, 1, 1, width, height, TRUE);
    const ScrollMetrics full = GetScrollMetrics(height);
    const bool needBar = height > 0 && width > barWidth && full.maxFirst > 0;

    if (needBar)
    {
        MoveWindow(g_hEdit, 1, 1, width - barWidth, height, TRUE);
        MoveWindow(g_hScroll, 1 + width - barWidth, 1, barWidth, height, TRUE);
        ShowWindow(g_hScroll, SW_SHOWNOACTIVATE);
        InvalidateRect(g_hScroll, nullptr, FALSE);
    }
    else
        ShowWindow(g_hScroll, SW_HIDE);
}

static void ScrollToPoint(int y, int offset)
{
    if (!g_hScroll || !g_hEdit) return;
    RECT rc{};
    GetClientRect(g_hScroll, &rc);
    const int height = rc.bottom - rc.top;
    ScrollMetrics m = GetScrollMetrics(height);
    if (m.maxFirst == 0) return;
    const int track = (std::max)(1, height - m.thumbHeight);
    int top = (std::max)(0, (std::min)(track, y - offset));
    int targetLine = (top * m.maxFirst + track / 2) / track;
    SendMessageW(g_hEdit, EM_LINESCROLL, 0, targetLine - m.first);
    InvalidateRect(g_hScroll, nullptr, FALSE);
}

static LRESULT CALLBACK ScrollBarProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const bool dark = IsDarkMode();
        HBRUSH track = CreateSolidBrush(dark ? RGB(30,30,30) : RGB(245,245,245));
        FillRect(dc, &rc, track);
        DeleteObject(track);
        ScrollMetrics m = GetScrollMetrics(rc.bottom - rc.top);
        if (m.maxFirst > 0)
        {
            RECT thumb{2, m.thumbTop, rc.right - 2, m.thumbTop + m.thumbHeight};
            HBRUSH brush = CreateSolidBrush(dark ?
                (GetCapture() == hwnd ? RGB(150,150,150) : RGB(95,95,95)) :
                (GetCapture() == hwnd ? RGB(130,130,130) : RGB(175,175,175)));
            FillRect(dc, &thumb, brush);
            DeleteObject(brush);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        ScrollMetrics m = GetScrollMetrics(rc.bottom - rc.top);
        int y = static_cast<short>(HIWORD(l));
        g_dragOffset = (y >= m.thumbTop && y < m.thumbTop + m.thumbHeight) ?
                       y - m.thumbTop : m.thumbHeight / 2;
        SetCapture(hwnd);
        ScrollToPoint(y, g_dragOffset);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd && g_dragOffset >= 0)
            ScrollToPoint(static_cast<short>(HIWORD(l)), g_dragOffset);
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (msg == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
        g_dragOffset = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        if (g_hEdit) SendMessageW(g_hEdit, WM_MOUSEWHEEL, w, l);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

/* ================= RichEdit 子类化：滚轮与用户选取自动复制 ================= */

static WNDPROC g_OrigEditProc = nullptr;

static std::wstring SelectedPlainText(HWND edit)
{
    if (!edit) return {};
    CHARRANGE selection{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    if (selection.cpMax <= selection.cpMin) return {};

    const size_t count = static_cast<size_t>(selection.cpMax - selection.cpMin);
    std::wstring value(count + 1, L'\0');
    const LRESULT copied = SendMessageW(edit, EM_GETSELTEXT, 0,
                                        reinterpret_cast<LPARAM>(&value[0]));
    if (copied <= 0 || static_cast<size_t>(copied) > count) return {};
    value.resize(static_cast<size_t>(copied));
    return value;
}

static void AutoCopySelection(HWND edit)
{
    if (!g_autoCopy) return;
    const std::wstring value = SelectedPlainText(edit);
    if (!value.empty())
        CopyClipboard(g_hWnd, value); // Plain Unicode text; no rich-text formatting.
}

static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEWHEEL:
    {
        // 即使隐藏滚动条，也确保滚轮能滚
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int linesPerNotch = 3; // 你想更快可改 5/8
        int lines = (delta / WHEEL_DELTA) * linesPerNotch;

        // delta>0 通常表示“滚轮向前”，内容应上移 => EM_LINESCROLL 传负数
        SendMessageW(hWnd, EM_LINESCROLL, 0, (LPARAM)(-lines));
        if (g_hScroll) InvalidateRect(g_hScroll, nullptr, FALSE);
        return 0;
    }
    }
    LRESULT result = CallWindowProcW(g_OrigEditProc, hWnd, msg, wParam, lParam);
    // Copy only on user selection-completion events. Never react to the
    // internal selection changes used for appending and highlighting output.
    if (msg == WM_LBUTTONUP ||
        (msg == WM_KEYUP &&
         (wParam == VK_SHIFT ||
          (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)))))
        AutoCopySelection(hWnd);
    if (g_hScroll && (msg == WM_KEYDOWN || msg == WM_VSCROLL ||
                      msg == WM_LBUTTONUP || msg == WM_SIZE))
        InvalidateRect(g_hScroll, nullptr, FALSE);
    return result;
}

/* ================= Dark Mode ================= */

bool IsDarkMode()
{
    HKEY hKey;
    DWORD v = 1, sz = sizeof(v);
    if (RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegQueryValueExW(
            hKey,
            L"AppsUseLightTheme",
            nullptr, nullptr,
            (LPBYTE)&v, &sz);
        RegCloseKey(hKey);
    }
    return v == 0;
}

static void MarkRange(LONG from, LONG to, COLORREF color)
{
    if (to <= from) return;
    CHARRANGE cr{from, to};
    SendMessageW(g_hEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    CHARFORMATW format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = color;
    SendMessageW(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
}

static bool IsAsciiWord(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
           (c >= L'0' && c <= L'9') || c == L'_' || c == L'.';
}

static void HighlightLine(const std::wstring& line, LONG base)
{
    const COLORREF green = IsDarkMode() ? RGB(115, 215, 145) : RGB(0, 125, 50);
    const COLORREF red = IsDarkMode() ? RGB(255, 110, 110) : RGB(185, 30, 30);
    const COLORREF yellow = IsDarkMode() ? RGB(240, 195, 95) : RGB(155, 95, 0);
    const COLORREF blue = IsDarkMode() ? RGB(135, 190, 235) : RGB(35, 100, 170);

    std::wstring upper = line;
    std::transform(upper.begin(), upper.end(), upper.begin(), towupper);

    // Restrict field names to the left side of a "FIELD : value" command-output line.
    static const wchar_t* fields[] = {
        L"SERVICE_NAME", L"STATE", L"TYPE", L"WIN32_EXIT_CODE",
        L"SERVICE_EXIT_CODE", L"CHECKPOINT", L"WAIT_HINT", L"PID", L"FLAGS"
    };
    size_t colon = upper.find(L':');
    if (colon != std::wstring::npos && colon < 30)
    {
        size_t start = upper.find_first_not_of(L" \t");
        if (start != std::wstring::npos && start < colon)
        {
            std::wstring key = upper.substr(start, colon - start);
            while (!key.empty() && iswspace(key.back())) key.pop_back();
            for (auto* field : fields)
                if (key == field) { MarkRange(base + static_cast<LONG>(start),
                     base + static_cast<LONG>(start + key.size()), blue); break; }
        }
    }

    struct Rule { const wchar_t* word; COLORREF color; bool ascii; };
    const Rule rules[] = {
        {L"RUNNING", green, true}, {L"SUCCESS", green, true},
        {L"SUCCEEDED", green, true}, {L"CONNECTED", green, true},
        {L"STOPPED", red, true}, {L"FAILED", red, true},
        {L"FAILURE", red, true}, {L"ERROR", red, true},
        {L"DISCONNECTED", red, true},
        {L"START_PENDING", yellow, true}, {L"STOP_PENDING", yellow, true},
        {L"WARNING", yellow, true}, {L"WARN", yellow, true},
        {L"成功", green, false}, {L"已启动", green, false},
        {L"完成", green, false}, {L"失败", red, false},
        {L"错误", red, false}, {L"已停止", red, false},
        {L"正在启动", yellow, false}, {L"启动中", yellow, false},
        {L"正在停止", yellow, false}, {L"等待", yellow, false}
    };
    for (const auto& rule : rules)
    {
        size_t wordlen = wcslen(rule.word);
        size_t at = 0;
        while ((at = upper.find(rule.word, at)) != std::wstring::npos)
        {
            bool boundary = !rule.ascii ||
                ((at == 0 || !IsAsciiWord(upper[at - 1])) &&
                 (at + wordlen == upper.size() || !IsAsciiWord(upper[at + wordlen])));
            if (boundary)
                MarkRange(base + static_cast<LONG>(at),
                          base + static_cast<LONG>(at + wordlen), rule.color);
            at += wordlen;
        }
    }

    // Preserve the original host + latency color coding (200/500 ms thresholds).
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (!iswdigit(line[i]) || (i && iswdigit(line[i-1]))) continue;
        size_t j = i;
        unsigned latency = 0;
        while (j < line.size() && iswdigit(line[j]))
        {
            latency = (std::min)(100000u, latency * 10 + static_cast<unsigned>(line[j] - L'0'));
            ++j;
        }
        if (j + 1 < line.size() && towlower(line[j]) == L'm' &&
            towlower(line[j + 1]) == L's')
        {
            COLORREF c = latency > 500 ? red : (latency >= 200 ? yellow : green);
            MarkRange(base + static_cast<LONG>(i), base + static_cast<LONG>(j + 2), c);
            size_t hostEnd = line.find_first_of(L" \t[");
            if (hostEnd != std::wstring::npos && hostEnd > 0 && hostEnd < 256)
                MarkRange(base, base + static_cast<LONG>(hostEnd), c);
            break;
        }
        i = j - 1;
    }
}

static void AppendLine(const std::wstring& line)
{
    GETTEXTLENGTHEX gtl{};
    gtl.flags = GTL_NUMCHARS;
    gtl.codepage = 1200;
    LONG base = static_cast<LONG>(SendMessageW(
        g_hEdit, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&gtl), 0));

    SendMessageW(g_hEdit, EM_SETSEL, -1, -1);
    CHARFORMATW normal{};
    normal.cbSize = sizeof(normal);
    normal.dwMask = CFM_COLOR;
    normal.crTextColor = IsDarkMode() ? RGB(220,220,220) : RGB(25,25,25);
    SendMessageW(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&normal));
    SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    HighlightLine(line, base);
}

void AppendText(const std::wstring& text)
{
    if (!g_hEdit || text.empty()) return;
    POINT oldScroll{};
    SendMessageW(g_hEdit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&oldScroll));
    const LRESULT first = SendMessageW(g_hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    const LRESULT lines = SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0);
    RECT rc{};
    GetClientRect(g_hEdit, &rc);
    HDC dc = GetDC(g_hEdit);
    HFONT display = g_displayFont ? g_displayFont :
        reinterpret_cast<HFONT>(SendMessageW(g_hEdit, WM_GETFONT, 0, 0));
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, display));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, oldFont);
    ReleaseDC(g_hEdit, dc);
    int visible = (std::max)(1, static_cast<int>((rc.bottom - rc.top) / (std::max)(1L, tm.tmHeight)));
    bool follow = (first + visible >= lines - 2);

    SendMessageW(g_hEdit, WM_SETREDRAW, FALSE, 0);
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size() - 1;
        AppendLine(text.substr(start, end - start + 1));
        start = end + 1;
    }

    if (follow)
    {
        SendMessageW(g_hEdit, EM_SETSEL, -1, -1);
        SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
    }
    else
        SendMessageW(g_hEdit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&oldScroll));
    SendMessageW(g_hEdit, WM_SETREDRAW, TRUE, 0);
    UpdateScrollLayout();
    if (follow)
    {
        SendMessageW(g_hEdit, EM_SETSEL, -1, -1);
        SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
    }
    InvalidateRect(g_hEdit, nullptr, FALSE);
    if (g_hScroll && IsWindowVisible(g_hScroll))
        InvalidateRect(g_hScroll, nullptr, FALSE);
}

void ApplyDarkTitleBar(HWND hwnd)
{
    BOOL dark = IsDarkMode();
    DwmSetWindowAttribute(
        hwnd,
        20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
        &dark,
        sizeof(dark));
}

void ApplyDarkRichEdit(HWND hEdit)
{
    bool dark = IsDarkMode();

    SendMessageW(
        hEdit,
        EM_SETBKGNDCOLOR,
        0,
        dark ? RGB(30,30,30) : RGB(255,255,255));

    CHARFORMATW cf{};
    cf.cbSize = sizeof(cf);
    // Theme changes must not reset a user-selected font or font size.
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = dark ? RGB(220,220,220) : RGB(0,0,0);
    SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

static int CurrentFontDpi()
{
    if (!g_hEdit) return 96;
    HDC dc = GetDC(g_hEdit);
    if (!dc) return 96;
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(g_hEdit, dc);
    return dpi > 0 ? dpi : 96;
}

static void ApplyDisplayFont()
{
    if (!g_hEdit) return;
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(static_cast<int>(g_fontSize), CurrentFontDpi(), 72);
    lf.lfWeight = static_cast<LONG>(g_fontWeight);
    lf.lfItalic = static_cast<BYTE>(g_fontItalic);
    lf.lfCharSet = DEFAULT_CHARSET;
    // RichEdit stores font family/size in CHARFORMAT but lacks a quality field.
    // WM_SETFONT supplies the requested rasterization quality to its host font.
    // Actual grayscale rendering depends on the RichEdit/GDI text engine:
    // this is opt-in so the unchanged ClearType path remains available.
    lf.lfQuality = g_grayAA ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, g_fontName.c_str(), LF_FACESIZE);

    HFONT newFont = CreateFontIndirectW(&lf);
    if (!newFont) return;

    CHARFORMATW cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC;
    cf.yHeight = static_cast<LONG>(g_fontSize * 20); // RichEdit uses twips.
    lstrcpynW(cf.szFaceName, g_fontName.c_str(), LF_FACESIZE);
    cf.dwEffects = (g_fontWeight >= FW_BOLD ? CFE_BOLD : 0) |
                   (g_fontItalic ? CFE_ITALIC : 0);

    CHARRANGE previousSelection{};
    POINT scroll{};
    SendMessageW(g_hEdit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&previousSelection));
    SendMessageW(g_hEdit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));

    SendMessageW(g_hEdit, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), FALSE);
    // SCF_ALL changes the existing report while retaining each span's color.
    // SCF_DEFAULT also controls the font of lines appended in the future.
    SendMessageW(g_hEdit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    SendMessageW(g_hEdit, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));
    SendMessageW(g_hEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&previousSelection));
    SendMessageW(g_hEdit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    SendMessageW(g_hEdit, WM_SETREDRAW, TRUE, 0);

    HFONT previousFont = g_displayFont;
    g_displayFont = newFont; // Used by the existing scrollbar/visible-line metrics.
    if (previousFont) DeleteObject(previousFont);
    UpdateScrollLayout();
    InvalidateRect(g_hEdit, nullptr, TRUE);
    if (g_hScroll) InvalidateRect(g_hScroll, nullptr, TRUE);
}

static void ChooseDisplayFont(HWND owner)
{
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(static_cast<int>(g_fontSize), CurrentFontDpi(), 72);
    lf.lfWeight = static_cast<LONG>(g_fontWeight);
    lf.lfItalic = static_cast<BYTE>(g_fontItalic);
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, g_fontName.c_str(), LF_FACESIZE);

    CHOOSEFONTW choice{};
    choice.lStructSize = sizeof(choice);
    choice.hwndOwner = owner;
    choice.lpLogFont = &lf;
    choice.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_LIMITSIZE;
    choice.nSizeMin = 6;
    choice.nSizeMax = 96;
    if (!ChooseFontW(&choice))
    {
        if (CommDlgExtendedError() != 0)
            MessageBoxW(owner, L"无法打开系统字体选择对话框。", L"ConsoleLoader", MB_ICONWARNING);
        return; // Cancel leaves all settings untouched.
    }

    g_fontName = lf.lfFaceName;
    g_fontSize = static_cast<DWORD>((std::max)(6L, (std::min)(96L,
                 static_cast<LONG>((choice.iPointSize + 5) / 10))));
    g_fontWeight = static_cast<DWORD>((std::max)(0L, (std::min)(1000L, lf.lfWeight)));
    g_fontItalic = lf.lfItalic ? 1 : 0;
    ApplyDisplayFont();
    if (!SaveSettings())
        MessageBoxW(owner, L"字体已应用，但无法保存到当前用户的注册表。", L"ConsoleLoader", MB_ICONWARNING);
}

/* ================= 工具 ================= */

std::wstring DecodeOutput(const std::string& bytes)
{
    if (bytes.empty()) return L"";
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, flags, bytes.data(),
                                static_cast<int>(bytes.size()), nullptr, 0);
    if (!n)
    {
        // Console output from legacy Chinese Windows tools normally uses the OEM code page.
        cp = GetOEMCP();
        flags = 0;
        n = MultiByteToWideChar(cp, flags, bytes.data(),
                                static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (!n) return L"[输出解码失败]\r\n";
    std::wstring result(n, L'\0');
    MultiByteToWideChar(cp, flags, bytes.data(), static_cast<int>(bytes.size()),
                        &result[0], n);
    return result;
}

static void PostOutput(const std::string& bytes)
{
    if (bytes.empty() || g_shuttingDown.load(std::memory_order_acquire)) return;
    auto* ws = new std::wstring(DecodeOutput(bytes));
    if (g_shuttingDown.load(std::memory_order_acquire) ||
        !PostMessageW(g_hWnd, WM_APPEND, 0, reinterpret_cast<LPARAM>(ws)))
        delete ws;
}

/* ================= 管道线程 ================= */

DWORD WINAPI PipeThread(LPVOID)
{
    char buf[4096];
    DWORD read = 0;
    std::string pending;
    while (!g_shuttingDown.load(std::memory_order_acquire) &&
           ReadFile(g_hRead, buf, sizeof(buf), &read, nullptr) && read)
    {
        if (g_shuttingDown.load(std::memory_order_acquire)) break;
        pending.append(buf, read);
        std::string batch;
        for (;;)
        {
            size_t pos = pending.find_first_of("\r\n");
            if (pos == std::string::npos) break;
            // Wait for the next read when CR might be the first half of CRLF.
            if (pending[pos] == '\r' && pos + 1 == pending.size()) break;
            size_t take = (pending[pos] == '\r' && pending[pos + 1] == '\n') ? 2 : 1;
            batch.append(pending, 0, pos);
            batch += "\r\n";
            pending.erase(0, pos + take);
        }
        PostOutput(batch);  // One UI message per pipe read, not one per output line.
    }
    if (!pending.empty() && !g_shuttingDown.load(std::memory_order_acquire))
    {
        if (pending.back() == '\r') pending.pop_back();
        pending += "\r\n";
        PostOutput(pending);
    }
    return 0;
}

/* ================= 窗口过程 ================= */

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        LoadSettings();
        LoadLibraryW(L"Msftedit.dll");

        // 设置主窗口图标（来自资源 IDI_APPICON）
        HICON hBig = (HICON)LoadImageW(GetModuleHandleW(nullptr), L"IDI_APPICON",
                                       IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
        HICON hSm  = (HICON)LoadImageW(GetModuleHandleW(nullptr), L"IDI_APPICON",
                                       IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

        SendMessageW(hWnd, WM_SETICON, ICON_BIG,   (LPARAM)hBig);
        SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hSm);

        // 关键改动：
        // 1) 去掉 WS_VSCROLL 隐藏滚动条
        // 2) 加 ES_DISABLENOSCROLL，确保“无滚动条”时仍允许滚动逻辑
        g_hEdit = CreateWindowExW(
            0,                    // ★ 不再用 WS_EX_CLIENTEDGE
            MSFTEDIT_CLASS,
            L"",
            WS_CHILD | WS_VISIBLE |
            ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | ES_DISABLENOSCROLL, // ★ 替换：去 WS_VSCROLL，加 ES_DISABLENOSCROLL
            1, 1, 0, 0,
            hWnd, nullptr, nullptr, nullptr);

        // RichEdit 子类化：只为滚轮支持，不改你现有输出逻辑
        g_OrigEditProc = (WNDPROC)SetWindowLongPtrW(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        g_hScroll = CreateWindowExW(0, L"ConsoleLoaderScrollbar", L"",
                                    WS_CHILD,
                                    0, 0, 12, 0, hWnd, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);

        HMENU systemMenu = GetSystemMenu(hWnd, FALSE);
        if (systemMenu)
        {
            AppendMenuW(systemMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(systemMenu, MF_STRING, IDM_COPY_SIZE, L"复制当前窗口尺寸");
            AppendMenuW(systemMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(systemMenu, MF_STRING, IDM_AUTO_COPY, L"选中后自动复制");
            AppendMenuW(systemMenu, MF_STRING, IDM_CHOOSE_FONT, L"字体与字号...");
            AppendMenuW(systemMenu, MF_STRING, IDM_GRAYSCALE_AA, L"灰度抗锯齿（实验）");
        }
        ApplyDarkTitleBar(hWnd);
        ApplyDarkRichEdit(g_hEdit);
        ApplyDisplayFont();
        return 0;
    }

    case WM_INITMENUPOPUP:
        if (reinterpret_cast<HMENU>(w) == GetSystemMenu(hWnd, FALSE))
        {
            RECT r{};
            GetWindowRect(hWnd, &r);
            std::wstring caption = L"复制窗口尺寸: " +
                std::to_wstring(r.right - r.left) + L"x" +
                std::to_wstring(r.bottom - r.top);
            ModifyMenuW(GetSystemMenu(hWnd, FALSE), IDM_COPY_SIZE,
                        MF_BYCOMMAND | MF_STRING, IDM_COPY_SIZE, caption.c_str());
            CheckMenuItem(GetSystemMenu(hWnd, FALSE), IDM_AUTO_COPY,
                          MF_BYCOMMAND | (g_autoCopy ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(GetSystemMenu(hWnd, FALSE), IDM_GRAYSCALE_AA,
                          MF_BYCOMMAND | (g_grayAA ? MF_CHECKED : MF_UNCHECKED));
        }
        break;

    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == IDM_GRAYSCALE_AA)
        {
            g_grayAA = !g_grayAA;
            ApplyDisplayFont();
            CheckMenuItem(GetSystemMenu(hWnd, FALSE), IDM_GRAYSCALE_AA,
                          MF_BYCOMMAND | (g_grayAA ? MF_CHECKED : MF_UNCHECKED));
            if (!SaveSettings())
                MessageBoxW(hWnd, L"抗锯齿模式已切换，但无法保存到注册表。",
                            L"ConsoleLoader", MB_ICONWARNING);
            return 0;
        }
        if ((w & 0xFFF0) == IDM_AUTO_COPY)
        {
            g_autoCopy = !g_autoCopy;
            CheckMenuItem(GetSystemMenu(hWnd, FALSE), IDM_AUTO_COPY,
                          MF_BYCOMMAND | (g_autoCopy ? MF_CHECKED : MF_UNCHECKED));
            if (!SaveSettings())
                MessageBoxW(hWnd, L"选中后自动复制已切换，但无法保存到注册表。",
                            L"ConsoleLoader", MB_ICONWARNING);
            return 0;
        }
        if ((w & 0xFFF0) == IDM_CHOOSE_FONT)
        {
            ChooseDisplayFont(hWnd);
            return 0;
        }
        if ((w & 0xFFF0) == IDM_COPY_SIZE)
        {
            RECT r{};
            GetWindowRect(hWnd, &r);
            std::wstring dimension = std::to_wstring(r.right - r.left) + L"x" +
                std::to_wstring(r.bottom - r.top);
            if (CopyClipboard(hWnd, dimension))
            {
                SetWindowTextW(hWnd, (g_baseTitle + L" [已复制 " + dimension + L"]").c_str());
                SetTimer(hWnd, TIMER_COPY_NOTICE, 2000, nullptr);
            }
            else MessageBoxW(hWnd, L"无法访问剪贴板。", L"ConsoleLoader", MB_ICONWARNING);
            return 0;
        }
        break;

    case WM_TIMER:
        if (w == TIMER_STARTUP_ACTIVATE)
        {
            if (++g_activationAttempts >= 5)
                KillTimer(hWnd, TIMER_STARTUP_ACTIVATE);
            ActivateStartupWindow(hWnd);
            return 0;
        }
        if (w == TIMER_CI_CLOSE)
        {
            KillTimer(hWnd, TIMER_CI_CLOSE);
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (w == TIMER_COPY_NOTICE)
        {
            KillTimer(hWnd, TIMER_COPY_NOTICE);
            SetWindowTextW(hWnd, g_baseTitle.c_str());
            return 0;
        }
        break;

    case WM_SETTINGCHANGE:
        ApplyDarkTitleBar(hWnd);
        ApplyDarkRichEdit(g_hEdit);
        UpdateScrollLayout();
        InvalidateRect(hWnd, nullptr, TRUE);
        if (g_hScroll && IsWindowVisible(g_hScroll))
            InvalidateRect(g_hScroll, nullptr, TRUE);
        return 0;

    case WM_SIZE:
        UpdateScrollLayout();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        // Match the RichEdit background exactly, including the 1px window
        // margins; no bright vertical seam or artificial border.
        HBRUSH background = CreateSolidBrush(
            IsDarkMode() ? RGB(30,30,30) : RGB(255,255,255));
        FillRect(dc, &rc, background);
        DeleteObject(background);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_APPEND:
    {
        auto* ws = (std::wstring*)l;
        AppendText(*ws);
        delete ws;
        return 0;
    }

    case WM_DESTROY:
    {
        KillTimer(hWnd, TIMER_COPY_NOTICE);
        KillTimer(hWnd, TIMER_CI_CLOSE);
        KillTimer(hWnd, TIMER_STARTUP_ACTIVATE);
        if (g_displayFont) { DeleteObject(g_displayFont); g_displayFont = nullptr; }

        // Stop the pipe worker *before* closing its read handle.
        // A detached GUI child can keep an inherited stdout pipe writer open,
        // so waiting for EOF is not a reliable shutdown mechanism.
        g_shuttingDown.store(true, std::memory_order_release);

        // Close the job to stop our direct console command. Detached GUI
        // children remain outside the job (SILENT_BREAKAWAY_OK).
        if (g_hJob) { CloseHandle(g_hJob); g_hJob = nullptr; }
        if (g_hProc) { CloseHandle(g_hProc); g_hProc = nullptr; }

        if (g_hPipeThread)
        {
            // Cancel blocking ReadFile on the worker. Retry also covers the
            // short window between checking the shutdown flag and ReadFile.
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                if (WaitForSingleObject(g_hPipeThread, 0) == WAIT_OBJECT_0)
                    break;
                CancelSynchronousIo(g_hPipeThread);
                if (WaitForSingleObject(g_hPipeThread, 50) == WAIT_OBJECT_0)
                    break;
            }

            // Do not close a pipe handle while another thread is using it.
            // Windows will close any remaining handles at process exit.
            const bool workerExited =
                WaitForSingleObject(g_hPipeThread, 0) == WAIT_OBJECT_0;
            CloseHandle(g_hPipeThread);
            g_hPipeThread = nullptr;
            if (workerExited && g_hRead)
            {
                CloseHandle(g_hRead);
                g_hRead = nullptr;
            }
        }
        else if (g_hRead)
        {
            CloseHandle(g_hRead);
            g_hRead = nullptr;
        }

        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, w, l);
}

/* ================= 入口 ================= */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    FreeConsole();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2)
    {
        if (argv) LocalFree(argv);
        MessageBoxW(nullptr,
            L"用法: ConsoleLoader.exe \"xxx.cmd\" [430x220]\n也支持目标程序及其参数.",
            L"ConsoleLoader", MB_ICONINFORMATION);
        return 1;
    }

    int width = 1280, height = 720;
    int sizeIndex = -1;
    int requestedW = 0, requestedH = 0;
    // The final argument is the original syntax. Windows Explorer's Send To
    // shortcuts append selected file paths AFTER shortcut arguments, so also
    // accept the window size immediately after the target executable.
    // Prefer the final position to preserve existing command lines.
    if (argc >= 3 && ParseWindowSize(argv[argc - 1], requestedW, requestedH))
        sizeIndex = argc - 1;
    else if (argc >= 3 && ParseWindowSize(argv[2], requestedW, requestedH))
        sizeIndex = 2;

    if (sizeIndex != -1)
    {
        width = requestedW;
        height = requestedH;
    }

    std::wstring target = argv[1];
    std::wstring args;
    for (int i = 2; i < argc; ++i)
    {
        if (i == sizeIndex) continue;
        if (!args.empty()) args += L" ";
        args += QuoteWinArg(argv[i]);
    }

    bool isBatch = EndsWithIgnoreCase(target, L".cmd") ||
                   EndsWithIgnoreCase(target, L".bat");
    std::wstring cmd;
    if (isBatch)
    {
        wchar_t comspec[32768]{};
        DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, 32768);
        if (!n || n >= 32768)
        {
            GetSystemDirectoryW(comspec, 32768);
            wcscat_s(comspec, 32768, L"\\cmd.exe");
        }
        // cmd /S /C requires doubled outer quotes around a quoted batch path.
        cmd = QuoteWinArg(comspec) + L" /D /S /C \"\"" + target + L"\"";
        if (!args.empty()) cmd += L" " + args;
        cmd += L"\"";
    }
    else
    {
        cmd = QuoteWinArg(target);
        if (!args.empty()) cmd += L" " + args;
    }
    LocalFree(argv);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ConsoleLoader";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW scrollClass{};
    scrollClass.lpfnWndProc = ScrollBarProc;
    scrollClass.hInstance = hInst;
    scrollClass.lpszClassName = L"ConsoleLoaderScrollbar";
    scrollClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&scrollClass);

    // Centre the window in the monitor work area containing the cursor.
    // The work area excludes the taskbar; explicit dimensions remain exact.
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    int left = CW_USEDEFAULT, top = CW_USEDEFAULT;
    if (GetMonitorInfoW(monitor, &monitorInfo))
    {
        const RECT& work = monitorInfo.rcWork;
        left = work.left + ((work.right - work.left) - width) / 2;
        top = work.top + ((work.bottom - work.top) - height) / 2;
    }

    g_baseTitle = L"ConsoleLoader - " + target;
    g_hWnd = CreateWindowW(wc.lpszClassName, g_baseTitle.c_str(),
            WS_OVERLAPPEDWINDOW, left, top,
            width, height, nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) return 1;

    // Show explicitly after the UAC-elevated process has started, rather than
    // relying on WS_VISIBLE during CreateWindow.
    ShowWindow(g_hWnd, SW_SHOWNORMAL);
    UpdateWindow(g_hWnd);
    ActivateStartupWindow(g_hWnd);
    // Bounded retry handles timing around the secure desktop handover.
    // Once foreground activation succeeds no further attempts are made.
    if (GetForegroundWindow() != g_hWnd)
        SetTimer(g_hWnd, TIMER_STARTUP_ACTIVATE, 300, nullptr);
    // WM_SIZE during CreateWindow precedes assignment to g_hWnd.
    // Lay out child controls once the main window handle is available.
    UpdateScrollLayout();

    // Regression-runner hook: GitHub's Windows service desktop cannot
    // enumerate GUI windows from the test process. This environment variable
    // makes the app deliver WM_CLOSE to itself; normal launches are unchanged.
    wchar_t ciCloseFlag[8]{};
    if (GetEnvironmentVariableW(L"CONSOLELOADER_CI_AUTOCLOSE", ciCloseFlag, 8) &&
        wcscmp(ciCloseFlag, L"1") == 0)
        SetTimer(g_hWnd, TIMER_CI_CLOSE, 2000, nullptr);

    g_hJob = CreateJobObjectW(nullptr, nullptr);
    if (g_hJob)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        // Clean up only the explicitly assigned console process. Child applications
        // launched by a batch script (for example 'start Proxifier.exe') escape
        // silently and must survive closing the ConsoleLoader window.
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        SetInformationJobObject(g_hJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hWrite = nullptr;
    if (!CreatePipe(&g_hRead, &hWrite, &sa, 0))
    {
        AppendText(L"ERROR: 创建输出管道失败: " + WinError(GetLastError()) + L"\r\n");
    }
    else
    {
        SetHandleInformation(g_hRead, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        BOOL started = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                   TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        DWORD launchError = started ? ERROR_SUCCESS : GetLastError();
        if (started)
        {
            g_hProc = pi.hProcess;
            if (g_hJob && !AssignProcessToJobObject(g_hJob, pi.hProcess))
                AppendText(L"WARNING: 无法将子进程加入 Job Object，关闭窗口可能无法终止它。\r\n");
            CloseHandle(pi.hThread);
        }
        CloseHandle(hWrite);
        if (!started)
            AppendText(L"ERROR: 启动失败: " + WinError(launchError) + L"\r\n命令: " + cmd + L"\r\n");
        else
        {
            g_hPipeThread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
            if (!g_hPipeThread)
                AppendText(L"ERROR: 无法创建输出读取线程: " + WinError(GetLastError()) + L"\r\n");
        }
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
