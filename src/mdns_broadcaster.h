#pragma once

#include "common.h"

// mDNS service advertisement.
//
// Without discovery, every client has to be told the IP address by hand.
// Advertising each configured printer over multicast DNS lets phones, macOS
// and Linux find the queues on their own.
//
// Each printer is published as its own instance so that the existing
// "one port per printer" layout keeps working: the service name carries the
// port, and the SRV record points at the host.

struct MdnsPrinterEntry {
    std::wstring printerName;
    int port = 9100;
    // "raw" publishes the classic pdl-datastream service, "xps" and "render"
    // publish the same service type but note the mode in the TXT record.
    std::wstring mode = L"auto";
    // When the LPD listener is active this carries its port so each printer
    // is additionally advertised as an RFC 1179 "_printer._tcp" service.
    // Android apps such as PrinterShare browse that type, while many of them
    // ignore "_pdl-datastream._tcp" entirely. 0 disables the announcement.
    int lpdPort = 0;
};

// Starts the broadcaster for the given printers. Calling it again replaces
// the advertised set.
bool StartMdnsBroadcaster(const std::vector<MdnsPrinterEntry>& printers,
                          std::wstring* errorText);

// Stops the broadcaster and releases the socket.
void StopMdnsBroadcaster();

// Reports whether the broadcaster is currently running.
bool IsMdnsBroadcasterRunning();
