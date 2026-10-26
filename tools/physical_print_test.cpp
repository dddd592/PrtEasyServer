// Physical print check for the render path.
//
// The program loads a document, sends it through the same steps the server
// uses, and submits it to a real Windows print queue. It prints what the
// queue answered, so the result can be compared against the paper.
//
// It is a console program on purpose: it can be run without starting the
// graphical interface.

#include <windows.h>
#include <winspool.h>

#include "render_engine.h"
#include "pdf_renderer.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Converts a wide string into narrow UTF-8 for the console output.
std::string NarrowForTest(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string buffer(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, buffer.data(), needed, nullptr, nullptr);
    if (!buffer.empty() && buffer.back() == '\0') {
        buffer.pop_back();
    }
    return buffer;
}

// Lists the installed printer queues. The names are returned as UTF-8 so
// that they can be printed on a console.
bool ListPrinters(std::vector<std::string>* names,
                  std::vector<std::wstring>* wideNames) {
    // The enumeration is done with a buffer that is large enough for every
    // queue. A probing call with a null buffer is rejected by the API.
    //
    // The output count is a byte count, so the number of entries follows
    // from dividing it by the size of one structure.
    std::vector<BYTE> buffer(64 * 1024);
    DWORD returnedBytes = 0;
    DWORD needed = 0;
    if (!::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2,
                          buffer.data(), static_cast<DWORD>(buffer.size()),
                          &returnedBytes, &needed)) {
        return false;
    }

    const auto* infos = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
    const std::size_t entryCount = returnedBytes / sizeof(PRINTER_INFO_2W);
    for (std::size_t i = 0; i < entryCount; ++i) {
        if (infos[i].pPrinterName == nullptr || infos[i].pPrinterName[0] == L'\0') {
            continue;
        }
        if (names != nullptr) {
            names->push_back(NarrowForTest(infos[i].pPrinterName));
        }
        if (wideNames != nullptr) {
            wideNames->push_back(infos[i].pPrinterName);
        }
    }
    return true;
}

// Reads the whole file into memory.
bool ReadFile(const char* path, std::vector<unsigned char>* data) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    data->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !data->empty();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "usage: physical_print_test <document> [printer-name]\n";
        std::cout << "       without a printer name the program lists the queues.\n";
        std::vector<std::string> names;
        if (ListPrinters(&names, nullptr)) {
            std::cout << "installed queues:" << std::endl;
            for (const std::string& name : names) {
                std::cout << "  " << name << std::endl;
            }
        } else {
            std::cout << "the printer queues could not be listed" << std::endl;
        }
        return 2;
    }

    if (argc < 3) {
        std::cout << "usage: physical_print_test <document> [printer-name]\n";
        return 2;
    }

    const std::string documentPath = argv[1];

    // The queue can be given by name or by its index in the system list.
    // The index form works for names that the console code page cannot
    // represent, and the wide name is what the printer API receives.
    std::wstring printerName;
    std::vector<std::wstring> wideNames;
    ListPrinters(nullptr, &wideNames);
    if (std::string(argv[2]).rfind("#", 0) == 0) {
        const int index = std::atoi(argv[2] + 1);
        if (index < 0 || static_cast<std::size_t>(index) >= wideNames.size()) {
            std::cout << "FAIL: the queue index is out of range" << std::endl;
            std::cout << "available queues:" << std::endl;
            for (std::size_t i = 0; i < wideNames.size(); ++i) {
                std::cout << "  #" << i << " " << NarrowForTest(wideNames[i]) << std::endl;
            }
            return 1;
        }
        printerName = wideNames[static_cast<std::size_t>(index)];
        std::cout << "queue: " << NarrowForTest(printerName) << std::endl;
    } else {
        printerName = Utf8ToWide(argv[2]);
    }

    std::vector<unsigned char> data;
    if (!ReadFile(documentPath.c_str(), &data)) {
        std::cout << "FAIL: the document could not be read: " << documentPath << std::endl;
        return 1;
    }
    std::cout << "document: " << documentPath << " (" << data.size() << " bytes)" << std::endl;

    // Report the state of the optional PDF library first, because it decides
    // whether a PDF document can be rasterised at all.
    std::cout << "pdf renderer: " << NarrowForTest(GetPdfRendererStatus()) << std::endl;

    const RenderJobKind kind = DetectRenderJobKind(data);
    std::cout << "payload type: " << (kind == RenderJobKind::Pdf ? "pdf"
                                    : kind == RenderJobKind::Image ? "image"
                                    : kind == RenderJobKind::Text ? "text"
                                    : kind == RenderJobKind::Xps ? "xps" : "unknown")
              << std::endl;

    std::vector<RenderPage> pages;
    std::wstring errorText;
    if (!SplitRenderPages(data, kind, &pages, &errorText)) {
        std::cout << "FAIL: the payload could not be prepared: " << NarrowForTest(errorText) << std::endl;
        return 1;
    }
    std::cout << "pages prepared: " << pages.size() << std::endl;

    RenderJobOptions options;
    options.copies = 1;
    options.fitToPage = true;

    const RenderResult result = RenderPagesToPrinter(pages, printerName, options);
    if (!result.ok) {
        std::cout << "FAIL: the queue refused the job: " << NarrowForTest(result.errorText) << std::endl;
        return 1;
    }

    std::cout << "PASS: the job was submitted to the queue" << std::endl;
    std::cout << "  pages submitted: " << result.pageCount << std::endl;
    if (!result.description.empty()) {
        std::cout << "  queue reported: " << NarrowForTest(result.description) << std::endl;
    }
    std::cout << std::endl;
    std::cout << "Check the paper by hand. A queue accepting a job does not prove" << std::endl;
    std::cout << "that the printer produced output." << std::endl;
    return 0;
}
