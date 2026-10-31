#pragma once

#include <cstdint>
#include <optional>

namespace pons {

// The PC's address on the local network (host byte order): a private IPv4
// address of a connected Wi-Fi or Ethernet adapter, preferring one with a
// default gateway (the router the phone is also on). Nothing when the PC is
// not on a private network.
std::optional<uint32_t> FindLocalAddress();

}  // namespace pons
