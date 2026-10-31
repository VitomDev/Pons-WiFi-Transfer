#include "core/network.h"

#include "core/http.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <vector>

namespace pons {

std::optional<uint32_t> FindLocalAddress() {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG result;
    do {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                                   GAA_FLAG_INCLUDE_GATEWAYS,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    } while (result == ERROR_BUFFER_OVERFLOW);
    if (result != NO_ERROR) return std::nullopt;

    std::optional<uint32_t> best;
    int bestScore = -1;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            adapter->IfType == IF_TYPE_TUNNEL) {
            continue;
        }
        // Virtual switches (Hyper-V, WSL, VPNs) usually have no gateway; the
        // real network does.
        int score = adapter->FirstGatewayAddress ? 2 : 0;
        if (adapter->IfType == IF_TYPE_IEEE80211 || adapter->IfType == IF_TYPE_ETHERNET_CSMACD) score += 1;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const uint32_t ip = ntohl(address->sin_addr.s_addr);
            if (!IsPrivateIPv4(ip)) continue;
            // Link-local addresses only when nothing better exists.
            const int addressScore = score + ((ip >> 16) == 0xA9FE ? 0 : 4);
            if (addressScore > bestScore) {
                bestScore = addressScore;
                best = ip;
            }
        }
    }
    return best;
}

}  // namespace pons
