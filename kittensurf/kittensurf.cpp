#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "framework.h"
#include "kittensurf.h"

#include <gdiplus.h>
#include <winhttp.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")

using namespace Gdiplus;

namespace
{
constexpr int kMaxLoadString = 100;
constexpr int kGoButtonId = 1;
constexpr UINT kPageLoadedMessage = WM_APP + 1;
constexpr std::size_t kMaxDocumentBytes = 16u * 1024u * 1024u;

constexpr wchar_t kLoadingPage[] = L"<html><body><p>正在加载...</p></body></html>";
constexpr wchar_t kEmptyPage[] = L"<html><body></body></html>";

HINSTANCE g_instance = nullptr;
WCHAR g_windowTitle[kMaxLoadString] = {};
WCHAR g_windowClass[kMaxLoadString] = {};
HWND g_urlBar = nullptr;
HWND g_goButton = nullptr;
ULONG_PTR g_gdiplusToken = 0;

struct PageState
{
    std::mutex mutex;
    std::condition_variable workersFinished;
    std::wstring html = kLoadingPage;
    std::uint64_t generation = 0;
    std::size_t activeWorkers = 0;
    bool closing = false;
};

PageState g_pageState;

class WinHttpHandle final
{
public:
    explicit WinHttpHandle(HINTERNET handle = nullptr) noexcept : handle_(handle) {}
    ~WinHttpHandle()
    {
        reset();
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HINTERNET get() const noexcept
    {
        return handle_;
    }

    explicit operator bool() const noexcept
    {
        return handle_ != nullptr;
    }

    void reset(HINTERNET handle = nullptr) noexcept
    {
        if (handle_ != nullptr)
        {
            WinHttpCloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HINTERNET handle_ = nullptr;
};

wchar_t lowerWide(wchar_t value)
{
    return static_cast<wchar_t>(std::towlower(value));
}

bool equalsInsensitive(const std::wstring& value, const wchar_t* expected)
{
    const std::size_t expectedLength = wcslen(expected);
    if (value.size() != expectedLength)
    {
        return false;
    }

    for (std::size_t i = 0; i < expectedLength; ++i)
    {
        if (lowerWide(value[i]) != lowerWide(expected[i]))
        {
            return false;
        }
    }
    return true;
}

bool startsWithInsensitive(const std::wstring& value, const wchar_t* prefix)
{
    const std::size_t prefixLength = wcslen(prefix);
    if (value.size() < prefixLength)
    {
        return false;
    }

    for (std::size_t i = 0; i < prefixLength; ++i)
    {
        if (lowerWide(value[i]) != lowerWide(prefix[i]))
        {
            return false;
        }
    }
    return true;
}

std::wstring trimWhitespace(const std::wstring& value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::iswspace(value[begin]) != 0)
    {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::iswspace(value[end - 1]) != 0)
    {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::wstring normalizeUrl(const std::wstring& value)
{
    const std::wstring trimmed = trimWhitespace(value);
    if (trimmed.empty())
    {
        return L"about:blank";
    }

    if (startsWithInsensitive(trimmed, L"http://") ||
        startsWithInsensitive(trimmed, L"https://") ||
        startsWithInsensitive(trimmed, L"about:"))
    {
        return trimmed;
    }

    // A hostname typed without a scheme is treated as HTTPS by default.
    return L"https://" + trimmed;
}

std::wstring readWindowText(HWND window)
{
    if (window == nullptr)
    {
        return {};
    }

    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
    {
        return {};
    }

    std::wstring value(static_cast<std::size_t>(length) + 1, L'\\0');
    const int copied = GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    value.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return value;
}

std::wstring makeErrorPage(const wchar_t* message)
{
    return std::wstring(L"<html><body><p>") + message + L"</p></body></html>";
}

std::wstring decodeResponse(const std::string& bytes)
{
    if (bytes.empty())
    {
        return {};
    }

    std::size_t offset = 0;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf)
    {
        offset = 3;
    }

    if (bytes.size() - offset > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }

    const char* data = bytes.data() + offset;
    const int length = static_cast<int>(bytes.size() - offset);
    int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, length, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;

    // A page may omit its charset declaration or use a legacy code page.
    if (wideLength <= 0)
    {
        codePage = CP_ACP;
        flags = 0;
        wideLength = MultiByteToWideChar(codePage, flags, data, length, nullptr, 0);
    }

    if (wideLength <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(wideLength), L'\\0');
    if (MultiByteToWideChar(codePage, flags, data, length, result.data(), wideLength) <= 0)
    {
        return {};
    }
    return result;
}

std::wstring fetchUrl(const std::wstring& url)
{
    if (!startsWithInsensitive(url, L"http://") &&
        !startsWithInsensitive(url, L"https://"))
    {
        return makeErrorPage(L"仅支持 http:// 和 https:// 地址");
    }

    if (url.size() > 32768 || url.size() >= static_cast<std::size_t>(std::numeric_limits<DWORD>::max()))
    {
        return makeErrorPage(L"URL 太长");
    }

    std::vector<wchar_t> hostBuffer(url.size() + 1, L'\\0');
    std::vector<wchar_t> pathBuffer(url.size() + 1, L'\\0');
    std::vector<wchar_t> extraBuffer(url.size() + 1, L'\\0');

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = hostBuffer.data();
    components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
    components.lpszUrlPath = pathBuffer.data();
    components.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());
    components.lpszExtraInfo = extraBuffer.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extraBuffer.size());

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components))
    {
        return makeErrorPage(L"URL 解析失败");
    }

    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS)
    {
        return makeErrorPage(L"不支持的 URL 协议");
    }

    if (components.dwHostNameLength == 0)
    {
        return makeErrorPage(L"URL 缺少主机名");
    }

    const std::wstring hostName(hostBuffer.data(), components.dwHostNameLength);
    std::wstring requestTarget(pathBuffer.data(), components.dwUrlPathLength);
    std::wstring extraInfo(extraBuffer.data(), components.dwExtraInfoLength);

    // Fragments are client-side only and must not be sent to the server.
    const std::size_t fragment = extraInfo.find(L'#');
    if (fragment != std::wstring::npos)
    {
        extraInfo.resize(fragment);
    }
    if (requestTarget.empty())
    {
        requestTarget = L"/";
    }
    requestTarget += extraInfo;

    WinHttpHandle session(WinHttpOpen(
        L"KittenSurf/1.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session)
    {
        return makeErrorPage(L"HTTP 初始化失败");
    }

    WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 10000);

    WinHttpHandle connection(WinHttpConnect(session.get(), hostName.c_str(), components.nPort, 0));
    if (!connection)
    {
        return makeErrorPage(L"连接失败");
    }

    const DWORD requestFlags = components.nScheme == INTERNET_SCHEME_HTTPS
        ? WINHTTP_FLAG_SECURE
        : 0;
    WinHttpHandle request(WinHttpOpenRequest(
        connection.get(),
        L"GET",
        requestTarget.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        requestFlags));
    if (!request)
    {
        return makeErrorPage(L"请求创建失败");
    }

    constexpr wchar_t requestHeaders[] =
        L"Accept: text/html, text/plain;q=0.9, */*;q=0.1\\r\\n"
        L"Accept-Encoding: identity\\r\\n";

    if (!WinHttpSendRequest(
        request.get(),
        requestHeaders,
        static_cast<DWORD>(-1L),
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0) || !WinHttpReceiveResponse(request.get(), nullptr))
    {
        return makeErrorPage(L"网络请求失败");
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
        request.get(),
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX))
    {
        return makeErrorPage(L"无法读取 HTTP 状态");
    }

    if (statusCode < 200 || statusCode >= 400)
    {
        return makeErrorPage(L"服务器返回了非成功状态");
    }

    std::string response;
    char buffer[8192];
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &bytesRead))
        {
            return makeErrorPage(L"读取响应失败");
        }
        if (bytesRead == 0)
        {
            break;
        }
        if (response.size() > kMaxDocumentBytes ||
            bytesRead > kMaxDocumentBytes - response.size())
        {
            return makeErrorPage(L"页面超过 16 MiB 限制");
        }
        response.append(buffer, bytesRead);
    }

    if (response.empty())
    {
        return makeErrorPage(L"服务器返回了空页面");
    }

    const std::wstring decoded = decodeResponse(response);
    return decoded.empty() ? makeErrorPage(L"页面编码无法识别") : decoded;
}

std::size_t findTagEnd(const std::wstring& html, std::size_t begin)
{
    wchar_t quote = 0;
    for (std::size_t i = begin; i < html.size(); ++i)
    {
        const wchar_t ch = html[i];
        if (quote != 0)
        {
            if (ch == quote)
            {
                quote = 0;
            }
        }
        else if (ch == L'\'' || ch == L'"')
        {
            quote = ch;
        }
        else if (ch == L'>')
        {
            return i;
        }
    }
    return std::wstring::npos;
}

std::wstring extractTagName(const std::wstring& rawTag)
{
    std::size_t begin = 0;
    while (begin < rawTag.size() && std::iswspace(rawTag[begin]) != 0)
    {
        ++begin;
    }
    if (begin < rawTag.size() && rawTag[begin] == L'/')
    {
        ++begin;
    }
    while (begin < rawTag.size() && std::iswspace(rawTag[begin]) != 0)
    {
        ++begin;
    }

    const std::size_t end = [&]()
    {
        std::size_t cursor = begin;
        while (cursor < rawTag.size() &&
               (std::iswalnum(rawTag[cursor]) != 0 || rawTag[cursor] == L'-' || rawTag[cursor] == L'_'))
        {
            ++cursor;
        }
        return cursor;
    }();

    std::wstring name = rawTag.substr(begin, end - begin);
    for (wchar_t& ch : name)
    {
        ch = lowerWide(ch);
    }
    return name;
}

bool isClosingTag(const std::wstring& rawTag)
{
    std::size_t begin = 0;
    while (begin < rawTag.size() && std::iswspace(rawTag[begin]) != 0)
    {
        ++begin;
    }
    return begin < rawTag.size() && rawTag[begin] == L'/';
}

bool findClosingRawTag(
    const std::wstring& html,
    std::size_t begin,
    const std::wstring& name,
    std::size_t& end)
{
    if (begin + 2 + name.size() > html.size() || html[begin] != L'<' || html[begin + 1] != L'/')
    {
        return false;
    }

    std::size_t nameBegin = begin + 2;
    while (nameBegin < html.size() && std::iswspace(html[nameBegin]) != 0)
    {
        ++nameBegin;
    }
    if (nameBegin + name.size() > html.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < name.size(); ++i)
    {
        if (lowerWide(html[nameBegin + i]) != name[i])
        {
            return false;
        }
    }

    const std::size_t afterName = nameBegin + name.size();
    if (afterName < html.size() &&
        std::iswspace(html[afterName]) == 0 &&
        html[afterName] != L'>' &&
        html[afterName] != L'/')
    {
        return false;
    }

    end = findTagEnd(html, begin);
    return end != std::wstring::npos;
}

bool isBlockTag(const std::wstring& name)
{
    static constexpr const wchar_t* blockTags[] =
    {
        L"address", L"article", L"aside", L"blockquote", L"body", L"br", L"caption",
        L"dd", L"div", L"dl", L"dt", L"fieldset", L"figcaption", L"figure", L"footer",
        L"form", L"h1", L"h2", L"h3", L"h4", L"h5", L"h6", L"head", L"header",
        L"hr", L"html", L"li", L"main", L"nav", L"ol", L"p", L"pre", L"section",
        L"table", L"tbody", L"td", L"tfoot", L"th", L"thead", L"title", L"tr", L"ul"
    };

    for (const wchar_t* blockTag : blockTags)
    {
        if (name == blockTag)
        {
            return true;
        }
    }
    return false;
}

void appendCodePoint(std::wstring& output, std::uint32_t codePoint)
{
    if (codePoint == 0 || codePoint > 0x10ffff ||
        (codePoint >= 0xd800 && codePoint <= 0xdfff))
    {
        output += L'\uFFFD';
        return;
    }

    if (sizeof(wchar_t) == 2 && codePoint > 0xffff)
    {
        codePoint -= 0x10000;
        output += static_cast<wchar_t>(0xd800 + (codePoint >> 10));
        output += static_cast<wchar_t>(0xdc00 + (codePoint & 0x3ff));
    }
    else
    {
        output += static_cast<wchar_t>(codePoint);
    }
}

bool parseNumericEntity(const std::wstring& token, std::uint32_t& value)
{
    if (token.size() < 2 || token[0] != L'#')
    {
        return false;
    }

    std::size_t begin = 1;
    int base = 10;
    if (begin < token.size() && (token[begin] == L'x' || token[begin] == L'X'))
    {
        base = 16;
        ++begin;
    }
    if (begin == token.size())
    {
        return false;
    }

    std::uint32_t parsed = 0;
    for (; begin < token.size(); ++begin)
    {
        const wchar_t ch = token[begin];
        int digit = -1;
        if (ch >= L'0' && ch <= L'9')
        {
            digit = ch - L'0';
        }
        else if (base == 16 && ch >= L'a' && ch <= L'f')
        {
            digit = ch - L'a' + 10;
        }
        else if (base == 16 && ch >= L'A' && ch <= L'F')
        {
            digit = ch - L'A' + 10;
        }
        if (digit < 0 || digit >= base || parsed > (0x10ffffu - static_cast<std::uint32_t>(digit)) / static_cast<std::uint32_t>(base))
        {
            return false;
        }
        parsed = parsed * static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(digit);
    }

    value = parsed;
    return true;
}

std::wstring decodeHtmlEntities(const std::wstring& value)
{
    std::wstring result;
    result.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] != L'&')
        {
            result += value[i];
            continue;
        }

        const std::size_t end = value.find(L';', i + 1);
        if (end == std::wstring::npos || end - i > 16)
        {
            result += value[i];
            continue;
        }

        const std::wstring token = value.substr(i + 1, end - i - 1);
        if (token == L"amp") result += L'&';
        else if (token == L"lt") result += L'<';
        else if (token == L"gt") result += L'>';
        else if (token == L"quot") result += L'"';
        else if (token == L"apos") result += L'\'';
        else if (token == L"nbsp") result += L'\u00A0';
        else
        {
            std::uint32_t codePoint = 0;
            if (!parseNumericEntity(token, codePoint))
            {
                result += L'&';
                continue;
            }
            appendCodePoint(result, codePoint);
        }
        i = end;
    }
    return result;
}

std::wstring normalizeVisibleText(const std::wstring& value)
{
    std::wstring result;
    bool pendingSpace = false;

    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const wchar_t ch = value[i];
        if (ch == L'\r' || ch == L'\n')
        {
            while (i + 1 < value.size() && (value[i + 1] == L'\r' || value[i + 1] == L'\n'))
            {
                ++i;
            }
            while (!result.empty() && result.back() == L' ')
            {
                result.pop_back();
            }
            if (!result.empty() && result.back() != L'\n')
            {
                result += L'\n';
            }
            pendingSpace = false;
        }
        else if (std::iswspace(ch) != 0 || ch == L'\u00A0')
        {
            pendingSpace = true;
        }
        else
        {
            if (pendingSpace && !result.empty() && result.back() != L'\n')
            {
                result += L' ';
            }
            result += ch;
            pendingSpace = false;
        }
    }

    while (!result.empty() && (result.back() == L' ' || result.back() == L'\n'))
    {
        result.pop_back();
    }
    return result;
}

std::wstring extractVisibleText(const std::wstring& html)
{
    std::wstring visible;
    visible.reserve(html.size());
    std::wstring rawTextTag;

    for (std::size_t i = 0; i < html.size();)
    {
        if (!rawTextTag.empty())
        {
            std::size_t tagEnd = 0;
            if (html[i] == L'<' && findClosingRawTag(html, i, rawTextTag, tagEnd))
            {
                rawTextTag.clear();
                i = tagEnd + 1;
            }
            else
            {
                ++i;
            }
            continue;
        }

        if (i + 3 < html.size() && html.compare(i, 4, L"<!--") == 0)
        {
            const std::size_t commentEnd = html.find(L"-->", i + 4);
            i = commentEnd == std::wstring::npos ? html.size() : commentEnd + 3;
            continue;
        }

        if (html[i] == L'<')
        {
            const std::size_t tagEnd = findTagEnd(html, i + 1);
            if (tagEnd == std::wstring::npos)
            {
                break;
            }

            const std::wstring rawTag = html.substr(i + 1, tagEnd - i - 1);
            const std::wstring name = extractTagName(rawTag);
            const bool closing = isClosingTag(rawTag);
            if (!closing && (name == L"script" || name == L"style" || name == L"template"))
            {
                rawTextTag = name;
            }
            if (isBlockTag(name))
            {
                visible += L'\n';
            }
            i = tagEnd + 1;
            continue;
        }

        visible += html[i++];
    }

    return normalizeVisibleText(decodeHtmlEntities(visible));
}

std::wstring currentPageContent()
{
    std::lock_guard<std::mutex> lock(g_pageState.mutex);
    return g_pageState.html;
}

std::uint64_t beginPageRequest()
{
    std::lock_guard<std::mutex> lock(g_pageState.mutex);
    if (g_pageState.closing)
    {
        return 0;
    }
    ++g_pageState.generation;
    g_pageState.html = kLoadingPage;
    return g_pageState.generation;
}

bool acceptPageContent(std::uint64_t generation, std::wstring html)
{
    std::lock_guard<std::mutex> lock(g_pageState.mutex);
    if (g_pageState.closing || generation != g_pageState.generation)
    {
        return false;
    }
    g_pageState.html = std::move(html);
    return true;
}

void workerStarted()
{
    std::lock_guard<std::mutex> lock(g_pageState.mutex);
    ++g_pageState.activeWorkers;
}

void workerFinished()
{
    std::lock_guard<std::mutex> lock(g_pageState.mutex);
    if (g_pageState.activeWorkers > 0)
    {
        --g_pageState.activeWorkers;
    }
    if (g_pageState.activeWorkers == 0)
    {
        g_pageState.workersFinished.notify_all();
    }
}

void stopPageLoading()
{
    std::unique_lock<std::mutex> lock(g_pageState.mutex);
    g_pageState.closing = true;
    ++g_pageState.generation;
    g_pageState.workersFinished.wait(lock, []
    {
        return g_pageState.activeWorkers == 0;
    });
}

void renderText(Graphics& graphics, const std::wstring& text, int x, int y, int maxWidth)
{
    if (maxWidth <= 0)
    {
        return;
    }

    FontFamily family(L"Microsoft YaHei");
    Font font(&family, 14.0f, FontStyleRegular, UnitPixel);
    SolidBrush brush(Color(255, 32, 32, 32));
    StringFormat format;
    format.SetAlignment(StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentNear);
    const RectF layoutRect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(maxWidth), 10000.0f);
    graphics.DrawString(text.c_str(), -1, &font, layoutRect, &format, &brush);
}

void loadUrlFromBar(HWND window)
{
    const std::wstring url = normalizeUrl(readWindowText(g_urlBar));
    SetWindowTextW(g_urlBar, url.c_str());

    const std::uint64_t generation = beginPageRequest();
    if (generation == 0)
    {
        return;
    }

    InvalidateRect(window, nullptr, TRUE);
    if (equalsInsensitive(url, L"about:blank"))
    {
        if (acceptPageContent(generation, kEmptyPage))
        {
            InvalidateRect(window, nullptr, TRUE);
        }
        return;
    }

    workerStarted();
    try
    {
        std::thread([window, url, generation]
        {
            const std::wstring page = fetchUrl(url);
            const bool accepted = acceptPageContent(generation, page);
            workerFinished();
            if (accepted)
            {
                PostMessageW(window, kPageLoadedMessage, 0, 0);
            }
        }).detach();
    }
    catch (...)
    {
        workerFinished();
        if (acceptPageContent(generation, makeErrorPage(L"无法创建网络线程")))
        {
            InvalidateRect(window, nullptr, TRUE);
        }
    }
}

ATOM registerWindowClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_KITTENSURF));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszMenuName = MAKEINTRESOURCEW(IDC_KITTENSURF);
    windowClass.lpszClassName = g_windowClass;
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
    return RegisterClassExW(&windowClass);
}

BOOL initializeInstance(HINSTANCE instance, int showCommand)
{
    g_instance = instance;
    HWND window = CreateWindowW(
        g_windowClass,
        g_windowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        0,
        900,
        650,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr)
    {
        return FALSE;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    return TRUE;
}
}

int APIENTRY wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE previousInstance,
    _In_ LPWSTR commandLine,
    _In_ int showCommand)
{
    UNREFERENCED_PARAMETER(previousInstance);
    UNREFERENCED_PARAMETER(commandLine);

    if (LoadStringW(instance, IDS_APP_TITLE, g_windowTitle, kMaxLoadString) == 0)
    {
        lstrcpyW(g_windowTitle, L"kittensurf");
    }
    if (LoadStringW(instance, IDC_KITTENSURF, g_windowClass, kMaxLoadString) == 0)
    {
        lstrcpyW(g_windowClass, L"KITTENSURF");
    }

    registerWindowClass(instance);
    if (!initializeInstance(instance, showCommand))
    {
        return FALSE;
    }

    HACCEL accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDC_KITTENSURF));
    MSG message{};
    int messageResult = 0;
    while ((messageResult = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        if (accelerators == nullptr || !TranslateAcceleratorW(message.hwnd, accelerators, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    return messageResult == -1 ? 1 : static_cast<int>(message.wParam);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        GdiplusStartupInput startupInput{};
        if (GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) != Ok)
        {
            g_gdiplusToken = 0;
        }

        g_urlBar = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"https://example.com/",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            10,
            10,
            700,
            28,
            window,
            nullptr,
            g_instance,
            nullptr);

        g_goButton = CreateWindowW(
            L"BUTTON",
            L"前往",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            720,
            10,
            70,
            28,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGoButtonId)),
            g_instance,
            nullptr);

        if (g_urlBar == nullptr || g_goButton == nullptr)
        {
            if (g_gdiplusToken != 0)
            {
                GdiplusShutdown(g_gdiplusToken);
                g_gdiplusToken = 0;
            }
            return -1;
        }

        loadUrlFromBar(window);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kGoButtonId:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                loadUrlFromBar(window);
                return 0;
            }
            break;
        case IDM_ABOUT:
            DialogBoxW(g_instance, MAKEINTRESOURCEW(IDD_ABOUTBOX), window, About);
            return 0;
        case IDM_EXIT:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);

    case WM_SIZE:
    {
        RECT clientRect{};
        GetClientRect(window, &clientRect);
        const int clientWidth = std::max(0L, clientRect.right - clientRect.left);
        constexpr int margin = 10;
        constexpr int buttonWidth = 70;
        const int buttonLeft = std::max(margin, clientWidth - margin - buttonWidth);
        const int barWidth = std::max(0, clientWidth - margin * 3 - buttonWidth);

        if (g_urlBar != nullptr)
        {
            SetWindowPos(g_urlBar, nullptr, margin, margin, barWidth, 28, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (g_goButton != nullptr)
        {
            SetWindowPos(g_goButton, nullptr, buttonLeft, margin, buttonWidth, 28, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case kPageLoadedMessage:
        InvalidateRect(window, nullptr, TRUE);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window, &paint);
        RECT clientRect{};
        GetClientRect(window, &clientRect);
        const std::wstring page = currentPageContent();
        std::wstring text = extractVisibleText(page);
        if (text.empty())
        {
            text = L"页面内容为空（可能包含动态脚本）";
        }

        if (g_gdiplusToken != 0)
        {
            Graphics graphics(deviceContext);
            graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            graphics.Clear(Color(255, 255, 255, 255));
            renderText(graphics, text, 20, 55, std::max(0L, clientRect.right - 40));
        }
        else
        {
            FillRect(deviceContext, &clientRect, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            TextOutW(deviceContext, 20, 55, text.c_str(), static_cast<int>(text.size()));
        }

        EndPaint(window, &paint);
        return 0;
    }

    case WM_DESTROY:
        stopPageLoading();
        if (g_gdiplusToken != 0)
        {
            GdiplusShutdown(g_gdiplusToken);
            g_gdiplusToken = 0;
        }
        g_urlBar = nullptr;
        g_goButton = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

INT_PTR CALLBACK About(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return static_cast<INT_PTR>(TRUE);
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(dialog, LOWORD(wParam));
            return static_cast<INT_PTR>(TRUE);
        }
        break;
    default:
        break;
    }
    return static_cast<INT_PTR>(FALSE);
}
