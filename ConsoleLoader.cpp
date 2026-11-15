#define UNICODE
#define _UNICODE

#include <windows.h>
#include <dwmapi.h>
#include <richedit.h>
#include <string>
#include <cwctype>
#include <shellapi.h>
#include <algorithm>
#include <vector>
#include <limits>
#include <cstdlib>

#pragma comment(lib, "dwmapi.lib")

/* ================= 全局（核心原样） ================= */

HWND   g_hWnd  = nullptr;
HWND   g_hEdit = nullptr;
HANDLE g_hRead = nullptr;
HANDLE g_hProc = nullptr;
HANDLE g_hJob  = nullptr;

#define WM_APPEND (WM_APP + 1)
#define IDM_COPY_SIZE 0x1F00
#define TIMER_COPY_NOTICE 7

std::wstring g_baseTitle;

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

/* ================= 仅为滚轮支持新增：RichEdit 子类化 ================= */

static WNDPROC g_OrigEditProc = nullptr;

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
        return 0;
    }
    }
    return CallWindowProcW(g_OrigEditProc, hWnd, msg, wParam, lParam);
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
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(
        dc, reinterpret_cast<HGDIOBJ>(SendMessageW(g_hEdit, WM_GETFONT, 0, 0))));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, oldFont);
    ReleaseDC(g_hEdit, dc);
    int visible = (std::max)(1, (rc.bottom - rc.top) / (std::max)(1L, tm.tmHeight));
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
    InvalidateRect(g_hEdit, nullptr, FALSE);
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
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    cf.crTextColor = dark ? RGB(220,220,220) : RGB(0,0,0);
    lstrcpyW(cf.szFaceName, L"Consolas");
    cf.yHeight = 180; // 9pt

    SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
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
    if (bytes.empty()) return;
    auto* ws = new std::wstring(DecodeOutput(bytes));
    if (!PostMessageW(g_hWnd, WM_APPEND, 0, reinterpret_cast<LPARAM>(ws))) delete ws;
}

/* ================= 管道线程 ================= */

DWORD WINAPI PipeThread(LPVOID)
{
    char buf[4096];
    DWORD read = 0;
    std::string pending;
    while (ReadFile(g_hRead, buf, sizeof(buf), &read, nullptr) && read)
    {
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
    if (!pending.empty())
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

        HMENU systemMenu = GetSystemMenu(hWnd, FALSE);
        if (systemMenu)
        {
            AppendMenuW(systemMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(systemMenu, MF_STRING, IDM_COPY_SIZE, L"复制当前窗口尺寸");
        }
        ApplyDarkTitleBar(hWnd);
        ApplyDarkRichEdit(g_hEdit);
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
        }
        break;

    case WM_SYSCOMMAND:
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
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;

    case WM_SIZE:
        MoveWindow(
            g_hEdit,
            1, 1,
            LOWORD(l) - 2,
            HIWORD(l) - 2,
            TRUE);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        if (IsDarkMode())
        {
            RECT rc;
            GetClientRect(hWnd, &rc);

            HBRUSH hBrush = CreateSolidBrush(RGB(55,55,55));
            FrameRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
        }

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
        KillTimer(hWnd, TIMER_COPY_NOTICE);
        if (g_hJob) { CloseHandle(g_hJob); g_hJob = nullptr; }
        if (g_hRead) { CloseHandle(g_hRead); g_hRead = nullptr; }
        if (g_hProc) { CloseHandle(g_hProc); g_hProc = nullptr; }
        PostQuitMessage(0);
        return 0;
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
            L"用法: ConsoleLoader.exe \"xxx.cmd\" [430x220]\\n也支持目标程序及其参数。",
            L"ConsoleLoader", MB_ICONINFORMATION);
        return 1;
    }

    int width = 1280, height = 720;
    int last = argc;
    int requestedW = 0, requestedH = 0;
    if (argc >= 3 && ParseWindowSize(argv[argc - 1], requestedW, requestedH))
    {
        width = requestedW; height = requestedH;
        --last;
    }

    std::wstring target = argv[1];
    std::wstring args;
    for (int i = 2; i < last; ++i)
    {
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
            wcscat_s(comspec, L"\\cmd.exe");
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

    g_baseTitle = L"ConsoleLoader - " + target;
    g_hWnd = CreateWindowW(wc.lpszClassName, g_baseTitle.c_str(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            width, height, nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) return 1;

    g_hJob = CreateJobObjectW(nullptr, nullptr);
    if (g_hJob)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
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
            HANDLE thread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
            else AppendText(L"ERROR: 无法创建输出读取线程: " + WinError(GetLastError()) + L"\r\n");
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
