// KittenSurf.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "KittenSurf.h"

#include <windows.h>
#include <objbase.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <sstream>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")

using namespace Gdiplus;

#define MAX_LOADSTRING 100

// 全局变量:
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
std::wstring g_htmlContent = L"<html><body><p>正在加载...</p></body></html>";

// 控件句柄
HWND g_hUrlBar = NULL;
HWND g_hGoButton = NULL;

// 函数前向声明:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

// ========================== 核心功能 ==========================

// 提取所有可见文本
std::wstring extractVisibleText(const std::wstring& html)
{
    std::wstring result;
    bool inTag = false;
    bool inScript = false;
    bool inStyle = false;
    bool inComment = false;

    for (size_t i = 0; i < html.length(); ++i)
    {
        wchar_t ch = html[i];

        if (!inComment && i + 3 < html.length() && html.substr(i, 4) == L"<!--")
        {
            inComment = true;
            continue;
        }
        if (inComment && i + 2 < html.length() && html.substr(i, 3) == L"-->")
        {
            inComment = false;
            i += 2;
            continue;
        }
        if (inComment) continue;

        if (!inTag && ch == L'<')
        {
            if (i + 6 < html.length() && html.substr(i, 7) == L"<script") inScript = true;
            if (i + 5 < html.length() && html.substr(i, 6) == L"<style") inStyle = true;
            inTag = true;
            continue;
        }

        if (inTag && ch == L'>')
        {
            inTag = false;
            if (inScript && i >= 8 && html.substr(i - 8, 9) == L"</script>") inScript = false;
            if (inStyle && i >= 7 && html.substr(i - 7, 8) == L"</style>") inStyle = false;
            continue;
        }

        if (inTag || inScript || inStyle) continue;
        result += ch;
    }

    std::wstring cleaned;
    bool lastWasSpace = false;
    for (wchar_t ch : result)
    {
        if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n')
        {
            if (!lastWasSpace)
            {
                cleaned += L' ';
                lastWasSpace = true;
            }
        }
        else
        {
            cleaned += ch;
            lastWasSpace = false;
        }
    }
    return cleaned;
}

// 渲染文本
void renderText(Graphics& graphics, const std::wstring& text, int x, int y, int maxWidth)
{
    Font font(L"微软雅黑", 14);
    SolidBrush brush(Color(0, 0, 0));
    RectF layoutRect((float)x, (float)y, (float)maxWidth, 10000.0f);
    graphics.DrawString(text.c_str(), -1, &font, layoutRect, NULL, &brush);
}

// WinHTTP 网络请求
std::wstring fetchURL(const std::wstring& url)
{
    std::wstring result;
    HINTERNET hSession = WinHttpOpen(L"KittenSurf/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return L"HTTP 初始化失败";

    URL_COMPONENTS urlComp = { 0 };
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256] = { 0 };
    wchar_t urlPath[1024] = { 0 };
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 1024;

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlComp))
    {
        WinHttpCloseHandle(hSession);
        return L"URL 解析失败";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return L"连接失败";
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL, NULL, NULL, flags);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"请求创建失败";
    }

    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WinHttpReceiveResponse(hRequest, NULL);

    DWORD bytesRead = 0;
    char buffer[4096];
    std::string utf8Data;
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
    {
        buffer[bytesRead] = '\0';
        utf8Data += buffer;
    }

    if (!utf8Data.empty())
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Data.c_str(), -1, NULL, 0);
        if (wlen > 0)
        {
            wchar_t* wbuf = new wchar_t[wlen];
            MultiByteToWideChar(CP_UTF8, 0, utf8Data.c_str(), -1, wbuf, wlen);
            result = wbuf;
            delete[] wbuf;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (result.empty())
    {
        result = L"<html><body><p>获取内容为空或请求失败</p></body></html>";
    }
    return result;
}

// 从网址栏加载内容
void loadUrlFromBar(HWND hWnd)
{
    wchar_t url[2048];
    GetWindowTextW(g_hUrlBar, url, 2048);
    if (wcslen(url) > 0)
    {
        std::wstring urlStr = url;
        if (urlStr.find(L"http://") == std::wstring::npos &&
            urlStr.find(L"https://") == std::wstring::npos)
        {
            urlStr = L"http://" + urlStr;
        }
        g_htmlContent = L"<html><body><p>正在加载...</p></body></html>";
        InvalidateRect(hWnd, NULL, TRUE);

        CreateThread(NULL, 0, [](LPVOID param) -> DWORD
            {
                HWND hwnd = (HWND)param;
                wchar_t url[2048];
                GetWindowTextW(g_hUrlBar, url, 2048);
                std::wstring urlStr = url;
                if (urlStr.find(L"http://") == std::wstring::npos &&
                    urlStr.find(L"https://") == std::wstring::npos)
                {
                    urlStr = L"http://" + urlStr;
                }
                std::wstring html = fetchURL(urlStr);
                g_htmlContent = html.empty() ? L"<html><body><p>网络请求失败</p></body></html>" : html;
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }, hWnd, 0, NULL);
    }
}

// ========================== Windows 标准函数 ==========================

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 初始化全局字符串
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_KITTENSURF, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // 执行应用程序初始化:
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_KITTENSURF));


    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int)msg.wParam;
}

//
//  函数: MyRegisterClass()
//  目标: 注册窗口类。
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_KITTENSURF));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_KITTENSURF);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   函数: InitInstance(HINSTANCE, int)
//   目标: 保存实例句柄并创建主窗口
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd)
    {
        return FALSE;
    }

    // ========== 加在这里 ==========
    HMENU hMenu = LoadMenuW(hInstance, MAKEINTRESOURCE(IDC_KITTENSURF));
    if (hMenu) SetMenu(hWnd, hMenu);
    // ==============================

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    return TRUE;
}

//
//  函数: WndProc(HWND, UINT, WPARAM, LPARAM)
//  目标: 处理主窗口的消息。
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static ULONG_PTR gdiplusToken;

    switch (message)
    {
    case WM_CREATE:
    {
        GdiplusStartupInput gdiplusStartupInput;
        GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

        // 创建网址栏
        g_hUrlBar = CreateWindowW(L"Edit", L"https://space.bilibili.com/1915578481/",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, 10, 400, 26, hWnd, NULL, hInst, NULL);

        // 创建跳转按钮
        g_hGoButton = CreateWindowW(L"Button", L"前往",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            420, 10, 60, 26, hWnd, (HMENU)1, hInst, NULL);

        // 初始加载
        loadUrlFromBar(hWnd);
        break;
    }

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);

        // 点击“前往”按钮（ID = 1）
        if (wmId == 1)
        {
            loadUrlFromBar(hWnd);
        }

        // 分析菜单选择:
        switch (wmId)
        {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    }

    case WM_SIZE:
    {
        RECT rect;
        GetClientRect(hWnd, &rect);
        if (g_hUrlBar && g_hGoButton)
        {
            int btnW = 70;
            int barWidth = rect.right - 20 - btnW;
            int btnLeft = rect.right - btnW - 10;
            SetWindowPos(g_hUrlBar, NULL, 10, 10, barWidth, 28, SWP_NOZORDER);
            SetWindowPos(g_hGoButton, NULL, btnLeft, 10, btnW, 28, SWP_NOZORDER);
        }
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        Graphics graphics(hdc);
        graphics.Clear(Color(255, 255, 255));

        RECT rect;
        GetClientRect(hWnd, &rect);
        std::wstring text = extractVisibleText(g_htmlContent);
        if (text.empty()) text = L"页面内容为空（可能包含大量动态脚本）";
        renderText(graphics, text, 20, 50, rect.right - 40);

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_DESTROY:
    {
        GdiplusShutdown(gdiplusToken);
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// “关于”框的消息处理程序。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
