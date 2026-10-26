// Standalone check for the PDFium dynamic loader.
//
// The main application loads pdfium.dll at run time, so the loader is
// verified here against a real DLL and a real PDF file. The test builds a
// small PDF in memory, rasterises it and reports the outcome.
//
// Usage:
//   pdf_renderer_test <path-to-pdfium.dll> [path-to-sample.pdf]
//
// The check script tools/test_pdf_renderer.sh calls this after downloading
// PDFium.

#include "pdf_renderer.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

// Builds a minimal single page PDF so the test does not depend on an
// external file. The content is a filled rectangle plus some text.
std::vector<unsigned char> BuildSamplePdf() {
    // The objects are emitted one by one while their byte offsets are
    // recorded, so that the cross reference table stays correct.
    std::string pdf = "%PDF-1.4\n";
    std::size_t positions[6] = {0, 0, 0, 0, 0, 0};
    const char* bodies[5] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length 78 >>\nstream\n0.9 0.9 0.2 rg 20 20 160 60 re f\n0 0 0 rg BT /F1 18 Tf 30 45 Td (PrtEasyServer) Tj ET\nendstream",
    };
    for (int index = 0; index < 5; ++index) {
        positions[index + 1] = pdf.size();
        pdf += std::to_string(index + 1) + " 0 obj\n";
        pdf += bodies[index];
        pdf += "\nendobj\n";
    }

    const std::size_t xrefOffset = pdf.size();
    pdf += "xref\n0 6\n";
    pdf += "0000000000 65535 f \n";
    for (int index = 1; index <= 5; ++index) {
        char entry[32] = {};
        std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", positions[index]);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n";
    pdf += std::to_string(xrefOffset);
    pdf += "\n%%EOF\n";

    return std::vector<unsigned char>(pdf.begin(), pdf.end());
}

bool LoadFile(const char* path, std::vector<unsigned char>* data) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    data->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // The DLL path argument is optional. The loader always looks next to the
    // executable, so the argument is only there to document the intent.
    if (argc > 2) {
        std::cout << "usage: pdf_renderer_test [sample.pdf]" << std::endl;
        return 2;
    }

    // The loader looks next to the executable, so the DLL is expected to sit
    // beside this test binary.
    std::wstring status = GetPdfRendererStatus();
    std::string narrowStatus;
    for (wchar_t ch : status) narrowStatus.push_back(static_cast<char>(ch));
    std::cout << "renderer status: " << narrowStatus << std::endl;
    if (!IsPdfRendererAvailable()) {
        std::cout << "FAIL: the PDFium library was not usable" << std::endl;
        return 1;
    }
    std::cout << "PASS: PDFium loaded" << std::endl;

    std::vector<unsigned char> pdf;
    if (argc >= 3) {
        if (!LoadFile(argv[2], &pdf)) {
            std::cout << "FAIL: the sample PDF could not be read" << std::endl;
            return 1;
        }
        std::cout << "PASS: loaded the sample document (" << pdf.size() << " bytes)" << std::endl;
    } else {
        pdf = BuildSamplePdf();
        std::cout << "PASS: built a sample document (" << pdf.size() << " bytes)" << std::endl;
    }

    std::vector<PdfPageBitmap> pages;
    std::wstring errorText;
    if (!RenderPdfPages(pdf, 300, &pages, &errorText)) {
        std::string narrowError;
        for (wchar_t ch : errorText) narrowError.push_back(static_cast<char>(ch));
        std::cout << "FAIL: " << narrowError << std::endl;
        return 1;
    }
    if (pages.empty()) {
        std::cout << "FAIL: no page was produced" << std::endl;
        return 1;
    }

    for (std::size_t index = 0; index < pages.size(); ++index) {
        const PdfPageBitmap& page = pages[index];
        if (page.width <= 0 || page.height <= 0 || page.pixels.empty()) {
            std::cout << "FAIL: page " << (index + 1) << " is empty" << std::endl;
            return 1;
        }
        const std::size_t expected = static_cast<std::size_t>(page.width) *
                                    static_cast<std::size_t>(page.height) * 4;
        if (page.pixels.size() != expected) {
            std::cout << "FAIL: page " << (index + 1) << " has an unexpected buffer size" << std::endl;
            return 1;
        }
        std::cout << "PASS: page " << (index + 1) << " rendered at "
                  << page.width << "x" << page.height << " pixels" << std::endl;
    }

    std::cout << "ALL CHECKS PASSED" << std::endl;
    return 0;
}
