#include "print_api.h"

#include <cstdlib>

#include <winsock2.h>

namespace {

// Decodes percent escapes and the "+" form of a space.
std::string UrlDecode(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '+') {
            output.push_back(' ');
            continue;
        }
        if (ch == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            char* end = nullptr;
            const long code = std::strtol(hex.c_str(), &end, 16);
            if (end != nullptr && *end == '\0' && hex.size() == 2) {
                output.push_back(static_cast<char>(code));
                i += 2;
                continue;
            }
        }
        output.push_back(ch);
    }
    return output;
}

std::string ToLowerAscii(const std::string& value) {
    std::string output = value;
    for (char& ch : output) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return output;
}

bool ParseBoolParameter(const std::string& value, bool defaultValue) {
    const std::string lowered = ToLowerAscii(value);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") return true;
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") return false;
    return defaultValue;
}

// Case-insensitive comparison used when matching a printer name.
bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right) {
    return ToLowerCopy(left) == ToLowerCopy(right);
}

}  // namespace

bool ParseHttpPrintRequest(const std::string& method,
                            const std::string& path,
                            const std::string& body,
                            const std::vector<std::wstring>& availablePrinters,
                            HttpPrintRequest* request,
                            std::wstring* errorText) {
    if (request == nullptr) {
        return false;
    }
    *request = HttpPrintRequest();

    if (method != "POST") {
        if (errorText) *errorText = L"Only POST is supported by the print API.";
        return false;
    }

    // Split the path from its query string.
    std::string pathOnly = path;
    std::string query;
    const std::size_t questionMark = path.find('?');
    if (questionMark != std::string::npos) {
        pathOnly = path.substr(0, questionMark);
        query = path.substr(questionMark + 1);
    }

    if (pathOnly != "/api/print") {
        if (errorText) *errorText = L"Unknown print API path.";
        return false;
    }

    // Parse the query parameters.
    std::size_t start = 0;
    while (start <= query.size()) {
        std::size_t separator = query.find('&', start);
        if (separator == std::string::npos) {
            separator = query.size();
        }
        const std::string pair = query.substr(start, separator - start);
        start = separator + 1;
        if (pair.empty()) {
            continue;
        }
        const std::size_t equals = pair.find('=');
        const std::string key = UrlDecode(equals == std::string::npos ? pair : pair.substr(0, equals));
        const std::string value = equals == std::string::npos ? std::string() : UrlDecode(pair.substr(equals + 1));
        const std::wstring wideKey = Utf8ToWide(key);
        const std::wstring wideValue = Utf8ToWide(value);

        if (wideKey == L"printer") {
            request->printerName = wideValue;
        } else if (wideKey == L"copies") {
            request->copies = std::atoi(value.c_str());
            if (request->copies < 1) request->copies = 1;
            if (request->copies > 999) request->copies = 999;
        } else if (wideKey == L"paper") {
            request->paperSize = wideValue;
        } else if (wideKey == L"orientation") {
            request->orientation = ToLowerCopy(wideValue);
        } else if (wideKey == L"fit") {
            request->fitToPage = ParseBoolParameter(value, true);
        } else if (wideKey == L"scale") {
            request->scalePercent = std::atof(value.c_str());
            if (request->scalePercent <= 0.0) request->scalePercent = 100.0;
            if (request->scalePercent > 400.0) request->scalePercent = 400.0;
        } else if (wideKey == L"monochrome") {
            request->monochrome = ParseBoolParameter(value, false);
        } else if (wideKey == L"fontsize") {
            request->fontSizePoints = std::atof(value.c_str());
            if (request->fontSizePoints <= 0.0) request->fontSizePoints = 11.0;
            if (request->fontSizePoints > 200.0) request->fontSizePoints = 200.0;
        } else if (wideKey == L"font") {
            request->fontFace = wideValue;
        }
    }

    if (request->printerName.empty()) {
        if (errorText) *errorText = L"The 'printer' parameter is required.";
        return false;
    }

    // Match the requested printer against the configured queues.
    bool matched = false;
    for (const std::wstring& candidate : availablePrinters) {
        if (EqualsIgnoreCase(candidate, request->printerName)) {
            request->printerName = candidate;
            matched = true;
            break;
        }
    }
    if (!matched) {
        if (errorText) {
            *errorText = L"Unknown printer: " + request->printerName;
        }
        return false;
    }

    if (body.empty()) {
        if (errorText) *errorText = L"The request carried no document data.";
        return false;
    }

    request->body.assign(body.begin(), body.end());
    return true;
}

std::string BuildPrintersJson(const std::vector<std::wstring>& printerNames) {
    std::string json = "{\"printers\":[";
    for (std::size_t i = 0; i < printerNames.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        json += "{\"name\":\"" + WideToUtf8(JsonEscape(printerNames[i])) + "\"}";
    }
    json += "]}";
    return json;
}

HttpRequestData ReadHttpRequestData(SOCKET socketHandle) {
    HttpRequestData result;
    // A stalled client must not hold the thread forever.
    const DWORD receiveTimeoutMs = 30000;
    ::setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&receiveTimeoutMs), sizeof(receiveTimeoutMs));

    std::string buffer;
    buffer.reserve(8192);

    const std::string headerTerminator = "\r\n\r\n";
    while (buffer.find(headerTerminator) == std::string::npos && buffer.size() < 65536) {
        char chunk[4096] = {};
        const int received = ::recv(socketHandle, chunk, sizeof(chunk), 0);
        if (received <= 0) {
            break;
        }
        buffer.append(chunk, chunk + received);
    }

    const std::size_t headerEnd = buffer.find(headerTerminator);
    if (headerEnd == std::string::npos) {
        result.head = buffer;
        result.complete = false;
        return result;
    }

    result.head = buffer.substr(0, headerEnd + headerTerminator.size());
    const std::string remainder = buffer.substr(headerEnd + headerTerminator.size());

    std::istringstream headStream(result.head);
    std::string version;
    headStream >> result.method >> result.path >> version;

    // Locate Content-Length and Transfer-Encoding in the header block.
    long long contentLength = 0;
    bool haveContentLength = false;
    std::size_t lineStart = result.head.find("\r\n");
    while (lineStart != std::string::npos && lineStart + 2 < result.head.size()) {
        std::size_t lineEnd = result.head.find("\r\n", lineStart + 2);
        if (lineEnd == std::string::npos) {
            lineEnd = result.head.size();
        }
        const std::string line = result.head.substr(lineStart + 2, lineEnd - lineStart - 2);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string key = ToLowerAscii(Trim(line.substr(0, colon)));
            const std::string value = Trim(line.substr(colon + 1));
            if (key == "content-length") {
                contentLength = std::atoll(value.c_str());
                haveContentLength = true;
            } else if (key == "transfer-encoding") {
                if (ToLowerAscii(value).find("chunked") != std::string::npos) {
                    result.chunked = true;
                }
            }
        }
        lineStart = lineEnd;
        if (lineEnd >= result.head.size()) {
            break;
        }
    }

    if (result.chunked) {
        // Chunked bodies are not decoded; the body stays empty and the
        // caller reports the limitation.
        result.body = remainder;
        result.hasBody = false;
        result.complete = false;
        return result;
    }

    if (!haveContentLength || contentLength <= 0) {
        result.body = remainder;
        result.hasBody = false;
        result.complete = true;
        return result;
    }

    // Refuse absurdly large bodies before allocating for them.
    constexpr long long kMaxBodyBytes = 256LL * 1024LL * 1024LL;  // 256 MiB
    if (contentLength > kMaxBodyBytes) {
        result.body = remainder;
        result.hasBody = false;
        result.complete = false;
        return result;
    }

    result.body = remainder;
    while (static_cast<long long>(result.body.size()) < contentLength) {
        char chunk[8192] = {};
        const int received = ::recv(socketHandle, chunk, sizeof(chunk), 0);
        if (received <= 0) {
            break;
        }
        result.body.append(chunk, chunk + received);
    }
    result.hasBody = true;
    result.complete = static_cast<long long>(result.body.size()) >= contentLength;
    return result;
}
