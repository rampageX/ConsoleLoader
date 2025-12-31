#define UNICODE
#define _UNICODE

#include <windows.h>
#include <dwmapi.h>
#include <richedit.h>
#include <string>

#pragma comment(lib, "dwmapi.lib")

/* ================= 全局（OK5 核心原样） ================= */

HWND   g_hWnd  = nullptr;
HWND   g_hEdit = nullptr;
HANDLE g_hRead = nullptr;
HANDLE g_hProc = nullptr;
HANDLE g_hJob  = nullptr;

#define WM_APPEND (WM_APP + 1)

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

/* ================= 工具（OK5 原样） ================= */

std::wstring AnsiToWideOEM(const char* s)
{
    int len = MultiByteToWideChar(GetOEMCP(), 0, s, -1, nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(GetOEMCP(), 0, s, -1, &ws[0], len);
    return ws;
}

void AppendText(const std::wstring& text)
{
    SendMessageW(g_hEdit, EM_SETSEL, -1, -1);
    SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
}

/* ================= 管道线程（OK5 原样） ================= */

DWORD WINAPI PipeThread(LPVOID)
{
    char buf[4096];
    DWORD read;
    std::string partial;

    while (ReadFile(g_hRead, buf, sizeof(buf)-1, &read, nullptr))
    {
        if (!read) break;
        partial.append(buf, read);

        while (true)
        {
            size_t rn = partial.find("\r\n");
            size_t r  = partial.find('\r');
            size_t n  = partial.find('\n');

            size_t pos = std::string::npos;
            size_t eat = 0;

            if (rn != std::string::npos)
            {
                pos = rn; eat = 2;
            }
            else if (r != std::string::npos && (n == std::string::npos || r < n))
            {
                pos = r; eat = 1;
            }
            else if (n != std::string::npos)
            {
                pos = n; eat = 1;
            }
            else
            {
                break;
            }

            std::string line = partial.substr(0, pos);
            partial.erase(0, pos + eat);

            // 统一成 CRLF，给 RichEdit 追加
            line += "\r\n";

            auto* ws = new std::wstring(AnsiToWideOEM(line.c_str()));
            PostMessageW(g_hWnd, WM_APPEND, 0, (LPARAM)ws);
        }

    }

    if (!partial.empty())
    {
        partial += "\r\n";
        auto* ws = new std::wstring(AnsiToWideOEM(partial.c_str()));
        PostMessageW(g_hWnd, WM_APPEND, 0, (LPARAM)ws);
    }
    return 0;
}

/* ================= 窗口过程（只改 UI） ================= */

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

        ApplyDarkTitleBar(hWnd);
        ApplyDarkRichEdit(g_hEdit);
        return 0;
    }

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
        if (g_hJob) CloseHandle(g_hJob);
        if (g_hRead) CloseHandle(g_hRead);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, w, l);
}

/* ================= 入口（OK5 核心原样） ================= */

int WINAPI wWinMain(
    HINSTANCE hInst,
    HINSTANCE,
    PWSTR lpCmdLine,
    int)
{
    // ★ 新增：释放 Explorer 自动分配的控制台
    FreeConsole();

    if (!lpCmdLine || !*lpCmdLine)
    {
        MessageBoxW(nullptr, L"请指定要运行的程序", L"ConsoleLoader", MB_ICONERROR);
        return 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"ConsoleLoader";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    std::wstring title = L"ConsoleLoader - ";
    title += lpCmdLine;

    // 默认尺寸
    const int baseW = 1280;
    const int baseH = 720;

    g_hWnd = CreateWindowW(
        wc.lpszClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        baseW, baseH,                 // ★ 改这里
        nullptr, nullptr, hInst, nullptr);

    /* ---------- Job Object ---------- */

    g_hJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    SetInformationJobObject(
        g_hJob,
        JobObjectExtendedLimitInformation,
        &jeli,
        sizeof(jeli));

    /* ---------- 管道 ---------- */

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hWrite = nullptr;
    CreatePipe(&g_hRead, &hWrite, &sa, 0);
    SetHandleInformation(g_hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = lpCmdLine;

    if (CreateProcessW(
        nullptr,
        &cmd[0],
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi))
    {
        g_hProc = pi.hProcess;
        AssignProcessToJobObject(g_hJob, pi.hProcess);
        CloseHandle(pi.hThread);
    }

    CloseHandle(hWrite);
    CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
