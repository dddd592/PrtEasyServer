#include "mdns_broadcaster.h"

#include <ws2tcpip.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#pragma comment(lib, "Ws2_32.lib")

namespace {

constexpr unsigned short kMdnsPort = 5353;
constexpr unsigned int kMdnsGroup = 0xE00000FB;  // 224.0.0.251 in host byte order
constexpr int kAnnounceIntervalSeconds = 30;
constexpr std::size_t kMaxPacketSize = 9000;

std::mutex gMutex;
SOCKET gSocket = INVALID_SOCKET;
std::thread gThread;
std::atomic<bool> gRunning{false};
std::vector<MdnsPrinterEntry> gPrinters;
std::wstring gHostName;

// Appends a DNS name in label form, e.g. "My Printer" -> 10"My Printer".
void AppendDnsName(std::string* buffer, const std::wstring& name) {
    std::string utf8 = WideToUtf8(name);
    if (utf8.size() > 63) {
        utf8.resize(63);
    }
    buffer->push_back(static_cast<char>(static_cast<unsigned char>(utf8.size())));
    buffer->append(utf8);
}

void AppendDnsName(std::string* buffer, const std::string& utf8Name) {
    std::string utf8 = utf8Name;
    if (utf8.size() > 63) {
        utf8.resize(63);
    }
    buffer->push_back(static_cast<char>(static_cast<unsigned char>(utf8.size())));
    buffer->append(utf8);
}

// Appends a fully qualified DNS name such as "_pdl-datastream._tcp.local".
void AppendDnsFqdn(std::string* buffer, const std::string& name) {
    std::size_t start = 0;
    while (start < name.size()) {
        const std::size_t dot = name.find('.', start);
        const std::size_t length = (dot == std::string::npos) ? (name.size() - start) : (dot - start);
        if (length == 0) {
            break;
        }
        buffer->push_back(static_cast<char>(static_cast<unsigned char>(length)));
        buffer->append(name, start, length);
        start = (dot == std::string::npos) ? name.size() : dot + 1;
    }
    buffer->push_back('\0');
}

void AppendUInt16(std::string* buffer, unsigned int value) {
    buffer->push_back(static_cast<char>((value >> 8) & 0xFF));
    buffer->push_back(static_cast<char>(value & 0xFF));
}

void AppendUInt32(std::string* buffer, unsigned int value) {
    buffer->push_back(static_cast<char>((value >> 24) & 0xFF));
    buffer->push_back(static_cast<char>((value >> 16) & 0xFF));
    buffer->push_back(static_cast<char>((value >> 8) & 0xFF));
    buffer->push_back(static_cast<char>(value & 0xFF));
}

// Builds a DNS record body for the given type and payload.
std::string BuildRecord(unsigned short type, unsigned int ttl, const std::string& payload) {
    std::string record;
    AppendUInt16(&record, type);
    AppendUInt16(&record, 0x0001);  // IN
    AppendUInt32(&record, ttl);
    AppendUInt16(&record, static_cast<unsigned int>(payload.size()));
    record.append(payload);
    return record;
}

// Builds a resource record. The owner name comes first, followed by the
// type, the class, the time to live and the record data. Unique records
// (SRV/TXT/A) set the cache-flush bit per RFC 6762 section 10.2.
std::string BuildResourceRecord(const std::string& ownerName,
                                 unsigned short type,
                                 bool cacheFlush,
                                 unsigned int ttl,
                                 const std::string& data) {
    std::string record;
    AppendDnsFqdn(&record, ownerName);
    AppendUInt16(&record, type);
    AppendUInt16(&record, cacheFlush ? 0x8001 : 0x0001);
    AppendUInt32(&record, ttl);
    AppendUInt16(&record, static_cast<unsigned int>(data.size()));
    record.append(data);
    return record;
}

// Builds the PTR record that maps the service type onto the instance.
// PTR is a shared record, so it keeps the plain IN class.
std::string BuildPtrRecord(const std::string& serviceType,
                            const std::string& instanceName,
                            unsigned int ttl) {
    std::string data;
    AppendDnsFqdn(&data, instanceName);
    return BuildResourceRecord(serviceType, 0x000C, false, ttl, data);
}

// Builds the SRV record that points at the host and the port.
std::string BuildSrvRecord(const std::string& instanceName,
                            const std::string& hostFqdn,
                            int port,
                            unsigned int ttl) {
    std::string data;
    AppendUInt16(&data, 0);  // priority
    AppendUInt16(&data, 0);  // weight
    AppendUInt16(&data, static_cast<unsigned int>(port));
    AppendDnsFqdn(&data, hostFqdn);
    return BuildResourceRecord(instanceName, 0x0021, true, ttl, data);
}

// Builds the TXT record. The key and value pairs follow the usual layout,
// where every entry is length prefixed.
std::string BuildTxtRecord(const std::string& instanceName,
                            const std::vector<std::string>& entries,
                            unsigned int ttl) {
    std::string data;
    for (const std::string& entry : entries) {
        data.push_back(static_cast<char>(static_cast<unsigned char>(entry.size())));
        data.append(entry);
    }
    if (data.empty()) {
        data.push_back('\0');
    }
    return BuildResourceRecord(instanceName, 0x0010, true, ttl, data);
}

// Builds the A record for the host address.
std::string BuildARecord(const std::string& hostFqdn,
                         const std::string& ipv4,
                         unsigned int ttl) {
    in_addr address{};
    if (::inet_pton(AF_INET, ipv4.c_str(), &address) != 1) {
        return std::string();
    }
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&address);
    std::string data(reinterpret_cast<const char*>(bytes), 4);
    return BuildResourceRecord(hostFqdn, 0x0001, true, ttl, data);
}

// Assembles the complete announcement packet for one printer.
//
// The packet carries four answers: the PTR record that ties the service
// type to the instance, the SRV record with the host and the port, the TXT
// record with the queue information and the A record with the address.
std::string BuildAnnouncementPacket(const MdnsPrinterEntry& entry,
                                    const std::string& hostName,
                                    const std::string& ipv4,
                                    const std::string& serviceType,
                                    int srvPort,
                                    const std::string& rpText) {
    // RFC 6763: the instance FQDN must embed the service type and the .local
    // domain. Browsers such as Android NSD / Mopria (used by Huawei phones)
    // reject partial instance names, so the suffix is not optional here.
    const std::string instanceName =
        WideToUtf8(entry.printerName) + " @ " + hostName + "." + serviceType;
    const std::string hostFqdn = hostName + ".local";

    std::vector<std::string> txtEntries;
    txtEntries.push_back("txtvers=1");
    txtEntries.push_back("rp=" + rpText);
    txtEntries.push_back("ty=" + WideToUtf8(entry.printerName));
    if (serviceType == "_pdl-datastream._tcp.local") {
        txtEntries.push_back("mode=" + WideToUtf8(entry.mode));
    }
    txtEntries.push_back("product=PrtEasyServer");

    const std::string aRecord = BuildARecord(hostFqdn, ipv4, 120);

    std::string packet;
    // A transaction id of zero marks an unsolicited multicast response.
    AppendUInt16(&packet, 0);
    // Standard query response, authoritative, no truncation.
    AppendUInt16(&packet, 0x8400);
    AppendUInt16(&packet, 0);      // questions
    AppendUInt16(&packet, aRecord.empty() ? 3 : 4);  // answers
    AppendUInt16(&packet, 0);      // authority
    AppendUInt16(&packet, 0);      // additional

    packet.append(BuildPtrRecord(serviceType, instanceName, 120));
    packet.append(BuildSrvRecord(instanceName, hostFqdn, srvPort, 120));
    packet.append(BuildTxtRecord(instanceName, txtEntries, 120));
    if (!aRecord.empty()) {
        packet.append(aRecord);
    }
    return packet;
}

void BroadcasterWorker() {
    while (gRunning.load()) {
        std::vector<MdnsPrinterEntry> printers;
        std::wstring hostName;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            printers = gPrinters;
            hostName = gHostName;
        }

        if (!printers.empty() && gSocket != INVALID_SOCKET) {
            const std::string ipv4 = WideToUtf8(GetLocalIPv4Address());
            if (!ipv4.empty()) {
                sockaddr_in destination{};
                destination.sin_family = AF_INET;
                destination.sin_port = htons(kMdnsPort);
                ::inet_pton(AF_INET, "224.0.0.251", &destination.sin_addr);

                for (const MdnsPrinterEntry& entry : printers) {
                    // Primary announcement: raw PDL datastream on the
                    // printer's own port.
                    const std::string pdlPacket = BuildAnnouncementPacket(
                        entry, WideToUtf8(hostName), ipv4,
                        "_pdl-datastream._tcp.local", entry.port,
                        std::to_string(entry.port) + "/" + WideToUtf8(entry.printerName));
                    if (!pdlPacket.empty() && pdlPacket.size() <= kMaxPacketSize) {
                        ::sendto(gSocket, pdlPacket.data(), static_cast<int>(pdlPacket.size()), 0,
                                 reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
                    }
                    // Secondary announcement: RFC 1179 LPD service. Apps such
                    // as PrinterShare browse "_printer._tcp" and often ignore
                    // "_pdl-datastream._tcp", so publishing both maximises the
                    // chance of being discovered.
                    if (entry.lpdPort > 0) {
                        const std::string lpdPacket = BuildAnnouncementPacket(
                            entry, WideToUtf8(hostName), ipv4,
                            "_printer._tcp.local", entry.lpdPort,
                            WideToUtf8(entry.printerName));
                        if (!lpdPacket.empty() && lpdPacket.size() <= kMaxPacketSize) {
                            ::sendto(gSocket, lpdPacket.data(), static_cast<int>(lpdPacket.size()), 0,
                                     reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
                        }
                    }
                }
            }
        }

        // Sleep in short slices so a stop request is handled quickly.
        for (int slice = 0; slice < kAnnounceIntervalSeconds * 10 && gRunning.load(); ++slice) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace

bool StartMdnsBroadcaster(const std::vector<MdnsPrinterEntry>& printers,
                          std::wstring* errorText) {
    StopMdnsBroadcaster();

    if (printers.empty()) {
        return true;
    }

    SOCKET socketHandle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        if (errorText) *errorText = L"mDNS socket could not be created: " + FormatLastErrorMessage();
        return false;
    }

    BOOL reuse = TRUE;
    ::setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(kMdnsPort);
    if (::bind(socketHandle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        // Another mDNS responder may already own the port. That is not
        // fatal: the printers are still reachable by IP.
        ::closesocket(socketHandle);
        if (errorText) {
            *errorText = L"mDNS port 5353 is already in use; discovery advertisement is disabled.";
        }
        return false;
    }

    // Join the multicast group on every interface so announcements leave the host.
    std::vector<unsigned long> addresses;
    ULONG size = 0;
    if (::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_UNICAST,
                               nullptr, nullptr, &size) == ERROR_BUFFER_OVERFLOW && size > 0) {
        std::vector<unsigned char> buffer(size);
        IP_ADAPTER_ADDRESSES* adapterList = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_UNICAST,
                                   nullptr, adapterList, &size) == NO_ERROR) {
            for (IP_ADAPTER_ADDRESSES* adapter = adapterList; adapter != nullptr; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp) {
                    continue;
                }
                for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
                     unicast != nullptr; unicast = unicast->Next) {
                    const sockaddr* socketAddress = unicast->Address.lpSockaddr;
                    if (socketAddress != nullptr && socketAddress->sa_family == AF_INET) {
                        addresses.push_back(reinterpret_cast<const sockaddr_in*>(socketAddress)->sin_addr.s_addr);
                    }
                }
            }
        }
    }

    for (unsigned long address : addresses) {
        ip_mreq membership{};
        membership.imr_multiaddr.s_addr = htonl(kMdnsGroup);
        membership.imr_interface.s_addr = address;
        ::setsockopt(socketHandle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     reinterpret_cast<const char*>(&membership), sizeof(membership));
    }

    // Pin the outgoing multicast interface to the same adapter the
    // announcements advertise. Leaving it at INADDR_ANY lets Windows choose a
    // default multicast interface, which may be a VPN virtual adapter (such as
    // aTrust); announcements would then never reach the LAN even though the
    // A record inside them points at the correct LAN address.
    unsigned long multicastInterface = htonl(INADDR_ANY);
    {
        const std::string localIp = WideToUtf8(GetLocalIPv4Address());
        in_addr chosen{};
        if (!localIp.empty() && ::inet_pton(AF_INET, localIp.c_str(), &chosen) == 1) {
            multicastInterface = chosen.s_addr;  // already in network byte order
        }
    }
    ::setsockopt(socketHandle, IPPROTO_IP, IP_MULTICAST_IF,
                 reinterpret_cast<const char*>(&multicastInterface), sizeof(multicastInterface));
    // RFC 6762 asks for a TTL of 255 so announcements reach the whole link.
    // The previous code reused the interface value (0), which confined the
    // packets to the local host and made discovery silently useless.
    const DWORD multicastTtl = 255;
    ::setsockopt(socketHandle, IPPROTO_IP, IP_MULTICAST_TTL,
                 reinterpret_cast<const char*>(&multicastTtl), sizeof(multicastTtl));

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSocket = socketHandle;
        gPrinters = printers;
        gHostName = GetComputerNameShort();
    }

    gRunning.store(true);
    gThread = std::thread(&BroadcasterWorker);

    if (errorText) {
        *errorText = L"mDNS advertisement started for " + std::to_wstring(printers.size()) + L" printer(s).";
    }
    return true;
}

void StopMdnsBroadcaster() {
    gRunning.store(false);
    if (gThread.joinable()) {
        gThread.join();
    }
    std::lock_guard<std::mutex> lock(gMutex);
    if (gSocket != INVALID_SOCKET) {
        ::closesocket(gSocket);
        gSocket = INVALID_SOCKET;
    }
    gPrinters.clear();
}

bool IsMdnsBroadcasterRunning() {
    return gRunning.load();
}
