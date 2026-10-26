#pragma once

#include "common.h"

// LPD / LPR support (RFC 1179).
//
// The RAW 9100 listener expects the client to speak the printer's own page
// description language. Many embedded devices, Unix hosts and older
// appliances only speak LPD, where the queue name selects the destination.
// This listener accepts those clients and maps the queue name onto one of
// the configured printers.
//
// The listener is a single TCP port. Each connection starts with a one byte
// command:
//
//   0x01  print a job, followed by a control file and then the data
//   0x02  print a job, data only
//   0x03  report the queue status
//   0x05  remove queued jobs
//   0x06  list the queues

struct LpdPrinterEntry {
    std::wstring printerName;
    std::wstring queueName;
    int port = 9100;
};

// Starts the LPD listener. Returns false and fills errorText on failure.
bool StartLpdListener(const std::vector<LpdPrinterEntry>& printers,
                       int listenPort,
                       std::wstring* errorText);

// Stops the LPD listener.
void StopLpdListener();

// Reports whether the LPD listener is running.
bool IsLpdListenerRunning();
