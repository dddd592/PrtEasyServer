#pragma once

#include <string>
#include <vector>

// Capability profile of an installed Windows printer driver, used by AUTO mode
// to pick a concrete transport (RAW / XPS / RENDER) at job time.
struct DriverProfile {
    bool driverInstalled = false;  // a DRIVER_INFO_6 was retrievable
    int emfCompliant = -1;        // -1 unknown, 0 no, 1 yes (DC_EMF_COMPLIANT)
    bool isPostScript = false;
    bool isPcl = false;
    bool isXpsCapable = false;
    bool isHostBased = false;     // present but no PDL and not XPS-capable
    std::wstring driverName;
};

namespace driver_probe {

// Probes the installed driver for the named printer queue and fills out a
// DriverProfile. Returns false only when the printer cannot be opened; a
// successful open with a missing driver still returns true with
// driverInstalled=false so callers can fall back safely.
bool ProbeDriverProfile(const std::wstring& printerName, DriverProfile* out, std::wstring* errorText);

// Short human-readable summary, e.g. "PCL6 + EMF-compatible", "PostScript",
// "Host-based (GDI)", "No driver installed", "Unknown (probe failed)".
std::wstring DescribeProfile(const DriverProfile& profile);

}  // namespace driver_probe
