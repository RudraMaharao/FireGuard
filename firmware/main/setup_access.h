#pragma once
#include <stdint.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace setup_access {
// Return host-order IPv4, including IPv4-mapped addresses on dual-stack sockets.
inline bool ipv4(const sockaddr *address, socklen_t length, uint32_t &host) {
    if (!address || length < sizeof(sockaddr))
        return false;
    if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        host = ntohl(reinterpret_cast<const sockaddr_in *>(address)->sin_addr.s_addr);
        return true;
    }
#if !defined(ESP_PLATFORM) || CONFIG_LWIP_IPV6
    if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto &v6 = reinterpret_cast<const sockaddr_in6 *>(address)->sin6_addr;
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&v6);
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(bytes, mapped, sizeof(mapped)))
            return false;
        uint32_t network;
        memcpy(&network, bytes + 12, sizeof(network));
        host = ntohl(network);
        return true;
    }
#endif
    return false;
}

inline bool allows(bool enabled, const sockaddr *local, socklen_t localLength,
                   const sockaddr *peer, socklen_t peerLength,
                   uint32_t apHost, uint32_t maskHost) {
    uint32_t localHost, peerHost;
    // Match the actual AP endpoint as well as its client subnet. An enabled AP
    // must never grant setup access to requests received on the station IP.
    return enabled && apHost && maskHost &&
           ipv4(local, localLength, localHost) && ipv4(peer, peerLength, peerHost) &&
           localHost == apHost && (peerHost & maskHost) == (apHost & maskHost);
}
} // namespace setup_access
