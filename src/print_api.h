#pragma once

#include "common.h"

// A minimal HTTP print API.
//
// The RAW path needs the client to speak PCL/PostScript, which rules out
// phones, macOS and most ERP systems. The HTTP print API accepts ordinary
// documents over plain HTTP instead, and the server renders them through the
// printer's own driver.
//
// Requests are handled as:
//
//   POST /api/print?printer=<name>&copies=1&paper=A4&orientation=portrait
//   Content-Type: application/octet-stream
//   <raw document bytes>
//
// The payload type is detected from the bytes themselves (image / text), so
// no driver is needed on the client.

struct HttpPrintRequest {
    std::wstring printerName;
    int copies = 1;
    std::wstring paperSize;
    std::wstring orientation;
    bool fitToPage = true;
    double scalePercent = 100.0;
    bool monochrome = false;
    double fontSizePoints = 11.0;
    std::wstring fontFace;
    std::vector<unsigned char> body;
};

// Parses the query string and body of a print request. Returns false and
// fills errorText when the request cannot be served.
bool ParseHttpPrintRequest(const std::string& method,
                            const std::string& path,
                            const std::string& body,
                            const std::vector<std::wstring>& availablePrinters,
                            HttpPrintRequest* request,
                            std::wstring* errorText);

// Builds the JSON body used by the /api/printers endpoint.
std::string BuildPrintersJson(const std::vector<std::wstring>& printerNames);

// Reads a complete HTTP request from a socket: the header block plus the
// body described by Content-Length. Chunked transfer encoding is not
// supported and is reported through the returned flag.
struct HttpRequestData {
    std::string head;      // header block including the request line
    std::string body;      // request body
    std::string method;
    std::string path;
    bool hasBody = false;
    bool chunked = false;
    bool complete = false;
};

HttpRequestData ReadHttpRequestData(SOCKET socketHandle);
