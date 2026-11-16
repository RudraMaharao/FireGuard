#include "setup_access.h"
#include <assert.h>
#include <stdio.h>

int main() {
    sockaddr_in local = {}, peer = {}, station = {}, outside = {};
    local.sin_family = peer.sin_family = station.sin_family = outside.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "192.168.4.1", &local.sin_addr) == 1);
    assert(inet_pton(AF_INET, "192.168.4.2", &peer.sin_addr) == 1);
    assert(inet_pton(AF_INET, "192.168.1.5", &station.sin_addr) == 1);
    assert(inet_pton(AF_INET, "10.0.0.2", &outside.sin_addr) == 1);
    sockaddr_in6 local6 = {}, peer6 = {}, native6 = {};
    local6.sin6_family = peer6.sin6_family = native6.sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "::ffff:192.168.4.1", &local6.sin6_addr) == 1);
    assert(inet_pton(AF_INET6, "::ffff:192.168.4.2", &peer6.sin6_addr) == 1);
    assert(inet_pton(AF_INET6, "fe80::1234", &native6.sin6_addr) == 1);
    constexpr uint32_t ap = 0xc0a80401, mask = 0xffffff00;
    auto addr = [](const auto &a) { return reinterpret_cast<const sockaddr *>(&a); };
    assert(setup_access::allows(true, addr(local), sizeof local, addr(peer), sizeof peer, ap, mask));
    // Regression: ESP-IDF creates a PF_INET6 listener with IPv4-mapped endpoints.
    assert(setup_access::allows(true, addr(local6), sizeof local6, addr(peer6), sizeof peer6, ap, mask));
    assert(setup_access::allows(true, addr(local6), sizeof local6, addr(peer), sizeof peer, ap, mask));
    assert(!setup_access::allows(false, addr(local), sizeof local, addr(peer), sizeof peer, ap, mask));
    assert(!setup_access::allows(true, addr(station), sizeof station, addr(peer), sizeof peer, ap, mask));
    assert(!setup_access::allows(true, addr(local), sizeof local, addr(outside), sizeof outside, ap, mask));
    assert(!setup_access::allows(true, addr(native6), sizeof native6, addr(peer6), sizeof peer6, ap, mask));
    assert(!setup_access::allows(true, addr(local), sizeof local, addr(peer6), sizeof(sockaddr), ap, mask));
    assert(!setup_access::allows(true, nullptr, 0, addr(peer), sizeof peer, ap, mask));
    assert(!setup_access::allows(true, addr(local), sizeof local, addr(peer), sizeof peer, 0, mask));
    assert(!setup_access::allows(true, addr(local), sizeof local, addr(peer), sizeof peer, ap, 0));
    puts("Setup access: IPv4/mapped-IPv6 AP clients accepted; station, disabled AP and invalid endpoints denied");
}
