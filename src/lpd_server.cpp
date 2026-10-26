#include "lpd_server.h"

#include <ws2tcpip.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#pragma comment(lib, "Ws2_32.lib")

namespace {

// Lowercases a narrow ASCII string for case-insensitive comparisons.
std::string ToLowerAscii(const std::string& value) {
    std::string output = value;
    for (char& ch : output) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return output;
}

std::mutex gMutex;
SOCKET gSocket = INVALID_SOCKET;
std::thread gThread;
std::atomic<bool> gRunning{false};
std::vector<LpdPrinterEntry> gPrinters;

// Reads exactly the requested number of bytes. Returns false on a short read.
bool ReadExact(SOCKET socketHandle, char* buffer, std::size_t length) {
    std::size_t total = 0;
    while (total < length) {
        const int received = ::recv(socketHandle, buffer + total, static_cast<int>(length - total), 0);
        if (received <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(received);
    }
    return true;
}

// Sends the whole buffer.
bool SendAll(SOCKET socketHandle, const char* data, std::size_t length) {
    std::size_t total = 0;
    while (total < length) {
        const int sent = ::send(socketHandle, data + total, static_cast<int>(length - total), 0);
        if (sent <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(sent);
    }
    return true;
}

// Reads a null terminated string.
bool ReadCString(SOCKET socketHandle, std::string* value, std::size_t limit = 1024) {
    value->clear();
    for (std::size_t i = 0; i < limit; ++i) {
        char ch = 0;
        if (!ReadExact(socketHandle, &ch, 1)) {
            return false;
        }
        if (ch == '\0') {
            return true;
        }
        value->push_back(ch);
    }
    return false;
}

// Reads the RFC 1179 job control file that follows a 0x01 command.
bool ReadControlFile(SOCKET socketHandle, std::string* control) {
    control->clear();
    while (true) {
        char line[1024] = {};
        std::size_t index = 0;
        while (index + 1 < sizeof(line)) {
            char ch = 0;
            if (!ReadExact(socketHandle, &ch, 1)) {
                return false;
            }
            if (ch == '\n') {
                break;
            }
            line[index++] = ch;
        }
        line[index] = '\0';
        if (index == 0) {
            // An empty line terminates the control file.
            return true;
        }
        control->append(line);
        control->append("\n");
        if (control->size() > 65536) {
            return false;
        }
    }
}

// Finds the queue that matches the requested name. The comparison ignores
// case and surrounding spaces because LPD clients vary in both.
bool FindQueue(const std::string& requested, LpdPrinterEntry* entry) {
    std::string wanted = requested;
    while (!wanted.empty() && (wanted.front() == ' ' || wanted.front() == '\t')) {
        wanted.erase(wanted.begin());
    }
    while (!wanted.empty() && (wanted.back() == ' ' || wanted.back() == '\t' || wanted.back() == '\r')) {
        wanted.pop_back();
    }

    std::lock_guard<std::mutex> lock(gMutex);
    for (const LpdPrinterEntry& candidate : gPrinters) {
        if (ToLowerAscii(WideToUtf8(candidate.queueName)) == ToLowerAscii(wanted)) {
            *entry = candidate;
            return true;
        }
    }
    // Fall back to the printer name so clients can use either identifier.
    for (const LpdPrinterEntry& candidate : gPrinters) {
        if (ToLowerAscii(WideToUtf8(candidate.printerName)) == ToLowerAscii(wanted)) {
            *entry = candidate;
            return true;
        }
    }
    return false;
}

// Sends the 0x03 queue status reply for one printer.
void SendQueueStatus(SOCKET socketHandle, const LpdPrinterEntry& entry) {
    std::string reply;
    reply.push_back('\0');  // 0 = accepted
    // job-id queue-name user size date time status
    std::string line = "000001 ";
    line += WideToUtf8(entry.queueName);
    line += " prteasy 0 ";
    // The timestamp format is fixed width; "Jan 01 00:00" style is accepted.
    line += "Jan 01 00:00 0\n";
    reply.append(line);
    ::send(socketHandle, reply.data(), static_cast<int>(reply.size()), 0);
}

// Sends the 0x06 queue list reply.
void SendQueueList(SOCKET socketHandle) {
    std::string reply;
    reply.push_back('\0');
    std::lock_guard<std::mutex> lock(gMutex);
    for (const LpdPrinterEntry& entry : gPrinters) {
        std::string line = WideToUtf8(entry.queueName);
        line += "\n";
        reply.append(line);
    }
    ::send(socketHandle, reply.data(), static_cast<int>(reply.size()), 0);
}

// Handles a single LPD connection.
void HandleLpdClient(SOCKET clientSocket) {
    // Bound every connection: stalled clients must not hold a thread
    // forever and payloads must not grow without limit.
    const DWORD receiveTimeoutMs = 30000;
    ::setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&receiveTimeoutMs), sizeof(receiveTimeoutMs));
    constexpr std::size_t kMaxPayloadBytes = 256ULL * 1024ULL * 1024ULL;  // 256 MiB

    char command = 0;
    if (!ReadExact(clientSocket, &command, 1)) {
        ::closesocket(clientSocket);
        return;
    }

    switch (command) {
        case 0x01: {
            // Print with a control file: "<queue>\n<control file>\n<data>"
            std::string queueName;
            if (!ReadCString(clientSocket, &queueName)) {
                break;
            }
            std::string control;
            if (!ReadControlFile(clientSocket, &control)) {
                break;
            }
            std::vector<unsigned char> payload;
            unsigned char chunk[8192] = {};
            bool payloadTooLarge = false;
            while (true) {
                const int received = ::recv(clientSocket, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
                if (received <= 0) {
                    break;
                }
                if (payload.size() + static_cast<std::size_t>(received) > kMaxPayloadBytes) {
                    payloadTooLarge = true;
                    payload.clear();
                    break;
                }
                payload.insert(payload.end(), chunk, chunk + received);
            }
            if (payloadTooLarge) {
                ::send(clientSocket, "\x01", 1, 0);
                break;
            }
            LpdPrinterEntry entry;
            if (!FindQueue(queueName, &entry)) {
                ::send(clientSocket, "\x01", 1, 0);
                break;
            }
            // The job is handed to the RAW listener of the target printer by
            // connecting to its own port.
            SOCKET printerSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (printerSocket != INVALID_SOCKET) {
                sockaddr_in destination{};
                destination.sin_family = AF_INET;
                destination.sin_port = htons(static_cast<unsigned short>(entry.port));
                destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (::connect(printerSocket, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0 &&
                    SendAll(printerSocket, reinterpret_cast<const char*>(payload.data()), payload.size())) {
                    // 0x00 acknowledges the job only after it was actually delivered.
                    ::send(clientSocket, "\0", 1, 0);
                } else {
                    // Report failure instead of pretending the job was printed.
                    ::send(clientSocket, "\x01", 1, 0);
                }
                ::closesocket(printerSocket);
            } else {
                ::send(clientSocket, "\x01", 1, 0);
            }
            break;
        }
        case 0x02: {
            // Print data only, without a control file.
            std::vector<unsigned char> payload;
            unsigned char chunk[8192] = {};
            bool payloadTooLarge = false;
            while (true) {
                const int received = ::recv(clientSocket, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
                if (received <= 0) {
                    break;
                }
                if (payload.size() + static_cast<std::size_t>(received) > kMaxPayloadBytes) {
                    payloadTooLarge = true;
                    payload.clear();
                    break;
                }
                payload.insert(payload.end(), chunk, chunk + received);
            }
            if (payloadTooLarge) {
                ::send(clientSocket, "\x01", 1, 0);
                break;
            }
            // Without a queue name the payload goes to the first printer.
            LpdPrinterEntry entry;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                if (gPrinters.empty()) {
                    break;
                }
                entry = gPrinters.front();
            }
            SOCKET printerSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (printerSocket != INVALID_SOCKET) {
                sockaddr_in destination{};
                destination.sin_family = AF_INET;
                destination.sin_port = htons(static_cast<unsigned short>(entry.port));
                destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (::connect(printerSocket, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0 &&
                    SendAll(printerSocket, reinterpret_cast<const char*>(payload.data()), payload.size())) {
                    // 0x00 acknowledges the job only after it was actually delivered.
                    ::send(clientSocket, "\0", 1, 0);
                } else {
                    // Report failure instead of pretending the job was printed.
                    ::send(clientSocket, "\x01", 1, 0);
                }
                ::closesocket(printerSocket);
            } else {
                ::send(clientSocket, "\x01", 1, 0);
            }
            break;
        }
        case 0x03: {
            std::string queueName;
            if (!ReadCString(clientSocket, &queueName)) {
                break;
            }
            LpdPrinterEntry entry;
            if (!FindQueue(queueName, &entry)) {
                ::send(clientSocket, "\x01", 1, 0);
                break;
            }
            SendQueueStatus(clientSocket, entry);
            break;
        }
        case 0x05: {
            std::string operatorName;
            std::string queueName;
            std::string jobNumbers;
            if (!ReadCString(clientSocket, &operatorName)) break;
            if (!ReadCString(clientSocket, &queueName)) break;
            if (!ReadCString(clientSocket, &jobNumbers)) break;
            // Jobs are printed immediately, so there is nothing to remove.
            ::send(clientSocket, "\0", 1, 0);
            break;
        }
        case 0x06: {
            SendQueueList(clientSocket);
            break;
        }
        default:
            break;
    }

    ::closesocket(clientSocket);
}

#ifdef _MSC_VER
// SEH guard: an access violation inside the LPD handler is a structured
// exception that C++ catch(...) cannot intercept under /EHsc. Convert it to
// a log-style console-free no-op: the connection is closed and the server
// keeps running. Kept free of C++ objects so C2712 cannot fire.
void HandleLpdClientGuarded(SOCKET clientSocket) {
    __try {
        HandleLpdClient(clientSocket);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ::closesocket(clientSocket);
    }
}
#endif

void AcceptLoop() {
    while (gRunning.load()) {
        sockaddr_in clientAddress{};
        int clientLength = sizeof(clientAddress);
        SOCKET clientSocket = ::accept(gSocket, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
        if (clientSocket == INVALID_SOCKET) {
            if (gRunning.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
#ifdef _MSC_VER
        std::thread(&HandleLpdClientGuarded, clientSocket).detach();
#else
        std::thread(&HandleLpdClient, clientSocket).detach();
#endif
    }
}

}  // namespace

bool StartLpdListener(const std::vector<LpdPrinterEntry>& printers,
                       int listenPort,
                       std::wstring* errorText) {
    StopLpdListener();

    if (printers.empty() || listenPort <= 0) {
        if (errorText) *errorText = L"The LPD listener was not started because no printers are configured.";
        return false;
    }

    SOCKET socketHandle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        if (errorText) *errorText = L"The LPD listener socket could not be created: " + FormatLastErrorMessage();
        return false;
    }

    BOOL reuse = TRUE;
    ::setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(listenPort));

    if (::bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::closesocket(socketHandle);
        if (errorText) {
            *errorText = L"The LPD listener could not bind to port " + std::to_wstring(listenPort) +
                         L": " + FormatLastErrorMessage();
        }
        return false;
    }
    if (::listen(socketHandle, SOMAXCONN) != 0) {
        ::closesocket(socketHandle);
        if (errorText) {
            *errorText = L"The LPD listener could not start listening on port " +
                         std::to_wstring(listenPort) + L": " + FormatLastErrorMessage();
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSocket = socketHandle;
        gPrinters = printers;
    }

    gRunning.store(true);
    gThread = std::thread(&AcceptLoop);

    if (errorText) {
        *errorText = L"The LPD listener is active on port " + std::to_wstring(listenPort) + L".";
    }
    return true;
}

void StopLpdListener() {
    gRunning.store(false);
    // Close the listening socket BEFORE joining the accept thread. The accept
    // thread is blocked inside accept(); on Windows closing the socket is what
    // makes that call return. Joining first (the previous order) deadlocked
    // the caller - and because the UI thread stops the server synchronously,
    // the whole window froze.
    SOCKET socketToClose = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        socketToClose = gSocket;
        gSocket = INVALID_SOCKET;
    }
    if (socketToClose != INVALID_SOCKET) {
        ::closesocket(socketToClose);
    }
    if (gThread.joinable()) {
        gThread.join();
    }
    std::lock_guard<std::mutex> lock(gMutex);
    gPrinters.clear();
}

bool IsLpdListenerRunning() {
    return gRunning.load();
}
