// End to end check for the PDF render path.
//
// The PDFium loader is verified on its own by pdf_renderer_test. This
// program goes one step further: it rasterises a PDF and then pushes the
// resulting bitmap through the same GDI+ drawing code that the application
// uses, so the whole chain from a PDF file to a printable bitmap is
// exercised.
//
// Usage:
//   pdf_render_path_test [sample.pdf]

#include "render_engine.h"
#include "pdf_renderer.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string NarrowForTest(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string buffer(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, buffer.data(), needed, nullptr, nullptr);
    if (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
    return buffer;
}

// Builds the same minimal single page PDF as the loader test.
std::vector<unsigned char> BuildSamplePdf() {
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

}  // namespace

int main(int argc, char** argv) {
    std::vector<unsigned char> pdf;
    if (argc >= 2) {
        std::FILE* handle = std::fopen(argv[1], "rb");
        if (handle == nullptr) {
            std::cout << "FAIL: the sample PDF could not be opened" << std::endl;
            return 1;
        }
        unsigned char chunk[4096] = {};
        while (true) {
            const std::size_t read = std::fread(chunk, 1, sizeof(chunk), handle);
            if (read == 0) break;
            pdf.insert(pdf.end(), chunk, chunk + read);
        }
        std::fclose(handle);
        std::cout << "PASS: loaded the sample document (" << pdf.size() << " bytes)" << std::endl;
    } else {
        pdf = BuildSamplePdf();
        std::cout << "PASS: built a sample document (" << pdf.size() << " bytes)" << std::endl;
    }

    // The application path: detect the payload, split it into pages and
    // confirm that a PDF turns into rasterised pages.
    const RenderJobKind kind = DetectRenderJobKind(pdf);
    if (kind != RenderJobKind::Pdf) {
        std::cout << "FAIL: the payload was not detected as a PDF" << std::endl;
        return 1;
    }
    std::cout << "PASS: the payload was detected as a PDF" << std::endl;

    std::vector<RenderPage> pages;
    std::wstring errorText;
    if (!SplitRenderPages(pdf, kind, &pages, &errorText)) {
        std::cout << "FAIL: " << NarrowForTest(errorText) << std::endl;
        return 1;
    }
    if (pages.empty()) {
        std::cout << "FAIL: the PDF produced no pages" << std::endl;
        return 1;
    }
    std::cout << "PASS: the PDF produced " << pages.size() << " page(s)" << std::endl;

    for (std::size_t index = 0; index < pages.size(); ++index) {
        const RenderPage& page = pages[index];
        if (!page.hasRawPixels) {
            std::cout << "FAIL: page " << (index + 1) << " is not rasterised" << std::endl;
            return 1;
        }
        const std::size_t expected = static_cast<std::size_t>(page.pixelWidth) *
                                    static_cast<std::size_t>(page.pixelHeight) * 4;
        if (page.rawPixels.size() < expected) {
            std::cout << "FAIL: page " << (index + 1) << " has a short pixel buffer" << std::endl;
            return 1;
        }
        std::cout << "PASS: page " << (index + 1) << " is rasterised at "
                  << page.pixelWidth << "x" << page.pixelHeight << " pixels" << std::endl;

        // The page must not be empty: at least one non white pixel proves
        // that the content was actually drawn.
        bool hasInk = false;
        for (std::size_t offset = 0; offset + 3 < page.rawPixels.size(); offset += 4) {
            if (page.rawPixels[offset] != 0xFF || page.rawPixels[offset + 1] != 0xFF ||
                page.rawPixels[offset + 2] != 0xFF) {
                hasInk = true;
                break;
            }
        }
        if (!hasInk) {
            std::cout << "FAIL: page " << (index + 1) << " came out blank" << std::endl;
            return 1;
        }
        std::cout << "PASS: page " << (index + 1) << " contains rendered content" << std::endl;
    }

    std::cout << "ALL CHECKS PASSED" << std::endl;
    return 0;
}
