#include "notice_client.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

namespace NoticeClient {

static const wchar_t* kHost = L"matepp.vercel.app";
static const wchar_t* kPath = L"/api/notice/eol";
static const int      kPort = INTERNET_DEFAULT_HTTPS_PORT;
static const DWORD    kTimeoutMs = 8000;

// UTF-8 -> UTF-16
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (len <= 0) return L"";
    std::wstring out(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

// Parse JSON string - đơn giản, không dùng Unicode escape
static bool ExtractJsonStringSimple(const std::wstring& json, const wchar_t* key, std::wstring& outVal) {
    std::wstring pattern = L"\"";
    pattern += key;
    pattern += L"\"";

    size_t keyPos = json.find(pattern);
    if (keyPos == std::wstring::npos) return false;

    size_t colonPos = json.find(L':', keyPos + pattern.size());
    if (colonPos == std::wstring::npos) return false;

    // Skip whitespace
    size_t i = colonPos + 1;
    while (i < json.size() && (json[i] == L' ' || json[i] == L'\t' ||
           json[i] == L'\n' || json[i] == L'\r')) i++;

    if (i >= json.size() || json[i] != L'"') return false;
    i++; // skip opening quote

    size_t start = i;
    bool escaped = false;

    while (i < json.size()) {
        if (escaped) {
            escaped = false;
            i++;
            continue;
        }
        if (json[i] == L'\\') {
            escaped = true;
            i++;
            continue;
        }
        if (json[i] == L'"') {
            break;
        }
        i++;
    }

    if (i >= json.size()) return false;

    outVal = json.substr(start, i - start);

    // Replace escape sequences (chỉ \n, \r, \t, \\, \")
    size_t pos = 0;
    while ((pos = outVal.find(L'\\', pos)) != std::wstring::npos) {
        if (pos + 1 < outVal.size()) {
            wchar_t c = outVal[pos + 1];
            switch (c) {
                case L'n': outVal.replace(pos, 2, L"\n"); break;
                case L'r': outVal.replace(pos, 2, L"\r"); break;
                case L't': outVal.replace(pos, 2, L"\t"); break;
                case L'\\': outVal.replace(pos, 2, L"\\"); break;
                case L'"': outVal.replace(pos, 2, L"\""); break;
                case L'/': outVal.replace(pos, 2, L"/"); break;
                default: pos += 2; continue;
            }
        }
        pos++;
    }

    return true;
}

EolResult FetchEolNotice() {
    EolResult res;

    // Use default title in English to avoid encoding issues
    res.title = L"Mate++ Wallpaper Engine - Notice";

    HINTERNET hSession = WinHttpOpen(
        L"MatePP-WallpaperEngine/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) {
        res.error = L"Cannot initialize WinHTTP session.";
        return res;
    }

    WinHttpSetTimeouts(hSession, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, kHost, kPort, 0);
    if (!hConnect) {
        res.error = L"Cannot connect to matepp.vercel.app.";
        WinHttpCloseHandle(hSession);
        return res;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", kPath,
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest) {
        res.error = L"Cannot create HTTPS request.";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return res;
    }

    BOOL sent = WinHttpSendRequest(
        hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    BOOL received = sent && WinHttpReceiveResponse(hRequest, NULL);

    if (!received) {
        DWORD err = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"Cannot fetch notice. (WinHTTP error %lu)", err);
        res.error = buf;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return res;
    }

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        wchar_t buf[256];
        swprintf_s(buf, L"Server error (HTTP %lu).", statusCode);
        res.error = buf;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return res;
    }

    // Read body
    std::string rawBody;
    DWORD avail = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
        if (avail == 0) break;

        std::vector<char> chunk(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
        rawBody.append(chunk.data(), read);
    } while (avail > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (rawBody.empty()) {
        res.error = L"Server returned empty data.";
        return res;
    }

    // Convert to wide string
    std::wstring json = Utf8ToWide(rawBody);

    // Parse fields - use simple parser
    std::wstring title, body;
    bool okTitle = ExtractJsonStringSimple(json, L"title", title);
    bool okBody = ExtractJsonStringSimple(json, L"body", body);

    // Use title from server if available and not empty
    if (okTitle && !title.empty()) {
        // Remove any invalid characters
        std::wstring cleanTitle;
        for (wchar_t c : title) {
            if (c >= 32 && c < 127) { // ASCII printable only
                cleanTitle += c;
            } else if (c == L' ' || c == L'-' || c == L'.') {
                cleanTitle += c;
            }
        }
        if (!cleanTitle.empty()) {
            res.title = L"Mate++ " + cleanTitle;
        }
    }

    if (okBody && !body.empty()) {
        res.body = body;
        // Remove any null characters
        res.body.erase(std::remove(res.body.begin(), res.body.end(), L'\0'), res.body.end());
    } else {
        res.body = L"No notice available at this time.";
    }

    res.ok = true;
    return res;
}

} // namespace NoticeClient
