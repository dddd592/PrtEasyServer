#include "driver_probe.h"

#include <windows.h>
#include <winspool.h>

#include "common.h"

#include <algorithm>

namespace {

bool ContainsCaseInsensitive(const std::wstring& haystack, const wchar_t* needle) {
    if (needle == nullptr) return false;
    const std::wstring h = ToLowerCopy(haystack);
    const std::wstring n = ToLowerCopy(needle);
    return h.find(n) != std::wstring::npos;
}

}  // namespace

namespace driver_probe {

bool ProbeDriverProfile(const std::wstring& printerName, DriverProfile* out, std::wstring* errorText) {
    if (out) *out = DriverProfile{};

    HANDLE printerHandle = nullptr;
    if (!::OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()), &printerHandle, nullptr)) {
        if (errorText) *errorText = L"OpenPrinter failed for " + printerName;
        return false;
    }

    DriverProfile profile;
    profile.driverInstalled = false;

    // Driver model name via GetPrinterDriver level 6 (DRIVER_INFO_6W::pName).
    DWORD needed = 0;
    ::GetPrinterDriverW(printerHandle, nullptr, 6, nullptr, 0, &needed);
    if (needed > 0) {
        std::vector<unsigned char> buffer(needed);
        if (::GetPrinterDriverW(printerHandle, nullptr, 6, buffer.data(), needed, &needed)) {
            DRIVER_INFO_6W* driver = reinterpret_cast<DRIVER_INFO_6W*>(buffer.data());
            if (driver->pName) {
                profile.driverName = driver->pName;
                profile.driverInstalled = true;
            }
        }
    }

    // Port name via GetPrinter level 2 (PRINTER_INFO_2W::pPortName) so that
    // DeviceCapabilities has a valid port to query against.
    std::wstring portName;
    DWORD portNeeded = 0;
    ::GetPrinterW(printerHandle, 2, nullptr, 0, &portNeeded);
    if (portNeeded > 0) {
        std::vector<unsigned char> portBuffer(portNeeded);
        if (::GetPrinterW(printerHandle, 2, portBuffer.data(), portNeeded, &portNeeded)) {
            PRINTER_INFO_2W* info = reinterpret_cast<PRINTER_INFO_2W*>(portBuffer.data());
            if (info->pPortName) {
                portName = info->pPortName;
            }
        }
    }

    // EMF compliance: DC_EMF_COMPLIANT == 1 means the driver is EMF-aware
    // (typically a PCL/PostScript class driver); 0 means host-based GDI.
    // Returns -1 on failure, which we treat as "unknown".
    const int emf = ::DeviceCapabilitiesW(printerName.c_str(),
                                         portName.empty() ? nullptr : portName.c_str(),
                                         DC_EMF_COMPLIANT, nullptr, nullptr);
    profile.emfCompliant = (emf == -1) ? -1 : emf;

    if (profile.driverInstalled) {
        const std::wstring name = profile.driverName;
        profile.isPostScript = ContainsCaseInsensitive(name, L"postscript");
        profile.isPcl = ContainsCaseInsensitive(name, L"pcl");
        profile.isXpsCapable = ContainsCaseInsensitive(name, L"xps") ||
                               ContainsCaseInsensitive(name, L"class driver");
        // Host-based: driver present but exposes no page-description language.
        profile.isHostBased = (profile.emfCompliant == 0) &&
                              !profile.isPostScript && !profile.isPcl && !profile.isXpsCapable;
    }

    ::ClosePrinter(printerHandle);

    if (out) *out = profile;
    return true;
}

std::wstring DescribeProfile(const DriverProfile& p) {
    if (!p.driverInstalled) return L"No driver installed";
    if (p.driverName.empty() && p.emfCompliant == -1) return L"Unknown (probe failed)";

    std::wstring summary;
    if (p.isPostScript) summary += L"PostScript";
    else if (p.isPcl) summary += L"PCL";
    else if (p.isXpsCapable) summary += L"XPS";
    else if (p.isHostBased) summary += L"Host-based (GDI)";
    else summary += L"Generic";

    if (p.emfCompliant == 1) summary += L" + EMF-compatible";
    else if (p.emfCompliant == 0) summary += L" (EMF-incompatible)";

    if (!p.driverName.empty()) summary += L" [" + p.driverName + L"]";
    return summary;
}

}  // namespace driver_probe
