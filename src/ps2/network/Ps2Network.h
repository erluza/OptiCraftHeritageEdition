#pragma once

#ifdef PS2_PLATFORM

#include <string>

namespace Ps2Network
{

// Initializes the PS2 network stack: loads DEV9/NETMAN/SMAP/PS2IP IRXs,
// brings up the interface, and configures DHCP.
// Returns true on success (network hardware ready).
bool initialize();

// Returns true if the network link is established and IP is configured.
bool isReady();

// Returns the active IP address string (e.g. "192.168.1.50") or empty string if not ready.
std::string getIpAddress();

struct DiagnosticResult
{
    bool hardwareOk = false;
    bool linkUp = false;
    bool hasIp = false;
    bool pingOk = false;
    int pingMs = -1;
    std::string ipAddress;
    std::string statusMessage;
};

// Tests hardware initialization, Ethernet link state, DHCP lease, and pings 8.8.8.8:53.
DiagnosticResult testConnection();

// Shuts down PS2IP and NETMAN services.
void shutdown();

} // namespace Ps2Network

#endif // PS2_PLATFORM
