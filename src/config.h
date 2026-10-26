#pragma once

#include "common.h"

struct PrinterConfigEntry {
    std::wstring printerName;
    int port = 9100;
    std::wstring mode = L"raw"; // "raw", "xps", "render" or "auto" (auto resolves per job)
};

// Maps a user supplied mode string onto one of the supported modes.
// Unknown values fall back to "raw" so older configurations keep working.
std::wstring NormalizePrintMode(const std::wstring& mode);

// Returns the display label for a mode value.
std::wstring PrintModeDisplayName(const std::wstring& mode);

struct AppConfig {
    std::vector<PrinterConfigEntry> printers;
    int webPort = 80;
    std::wstring language = L"en";
    bool minimizeToTray = true;
    // LPD (RFC 1179) listener settings. A port of 0 disables the listener.
    bool enableLpd = false;
    int lpdPort = 515;
    // mDNS service advertisement so clients can auto-discover the printers.
    bool enableMdns = true;
};

class ConfigStore {
public:
    ConfigStore();

    const std::wstring& GetConfigPath() const;
    AppConfig Load() const;
    bool Save(const AppConfig& config) const;

private:
    std::wstring configPath_;

    static std::wstring EscapeRegex(const std::wstring& input);
    static int ParseIntValue(const std::wstring& json, const std::wstring& key, int defaultValue);
    static bool ParseBoolValue(const std::wstring& json, const std::wstring& key, bool defaultValue);
    static std::wstring ParseStringValue(const std::wstring& json, const std::wstring& key, const std::wstring& defaultValue);
};
