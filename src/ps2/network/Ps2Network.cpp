#include "ps2/network/Ps2Network.h"

#ifdef PS2_PLATFORM

#include "platform/Log.h"
#include "ps2/system/Ps2IrxLoader.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#define LIBCGLUE_SYS_SOCKET_ALIASES 1
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <kernel.h>
#include <delaythread.h>

extern "C"
{
#include <netman.h>
#include <ps2ip.h>
#undef lwip_ioctl
int lwip_ioctl(int s, long cmd, void *argp);
}

namespace Ps2Network
{
namespace
{

std::mutex s_stateMutex;
bool s_initialized = false;
bool s_ready = false;
std::string s_ipAddress;

int applyLinkMode(int mode)
{
    static int currentMode = -1;
    if (currentMode != mode)
    {
        int res = NetManSetLinkMode(mode);
        if (res == 0)
            currentMode = mode;
        return res;
    }
    return 0;
}

bool checkLinkState()
{
    return NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, nullptr, 0, nullptr, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP;
}

} // namespace

bool initialize()
{
    std::lock_guard<std::mutex> guard(s_stateMutex);
    if (s_initialized)
        return s_ready;

    MC_LOG_INFO("network", "[PS2] Initializing network subsystem...\n");
    printf("[PS2 Network] Initializing network subsystem...\n");

    // 1. Load IOP network modules
    int r_dev9 = Ps2IrxLoader::load("irx/ps2dev9.irx", "host:ps2dev9.irx");
    int r_netman = Ps2IrxLoader::load("irx/netman.irx", "host:netman.irx");
    int r_smap = Ps2IrxLoader::load("irx/smap.irx", "host:smap.irx");
    int r_ps2ip = Ps2IrxLoader::load("irx/ps2ip.irx", "host:ps2ip.irx");

    if (r_dev9 < 0 || r_netman < 0 || r_smap < 0 || r_ps2ip < 0)
    {
        printf("[PS2 Network] IRX load failed (dev9=%d, netman=%d, smap=%d, ps2ip=%d)\n",
               r_dev9, r_netman, r_smap, r_ps2ip);
        MC_LOG_ERROR("network", "[PS2] Network modules failed to load (dev9=%d, netman=%d, smap=%d, ps2ip=%d)\n",
                     r_dev9, r_netman, r_smap, r_ps2ip);
        return false;
    }
    printf("[PS2 Network] IRX modules loaded OK\n");

    // 2. Initialize NETMAN service
    int res = NetManInit();
    if (res < 0)
    {
        printf("[PS2 Network] NetManInit failed (%d)\n", res);
        MC_LOG_ERROR("network", "[PS2] NetManInit failed (%d)\n", res);
        return false;
    }

    // 3. Set Ethernet link mode to auto-negotiation
    applyLinkMode(NETMAN_NETIF_ETH_LINK_MODE_AUTO);

    // 4. Initialize TCP/IP stack with 0.0.0.0 (let DHCP assign the real address).
    //    PCSX2 Sockets mode uses NAT with virtual subnet 192.0.2.0/24.
    //    Initializing with a static 192.168.x.x IP conflicts with the DHCP response.
    printf("[PS2 Network] Initializing TCP/IP stack (DHCP mode, start at 0.0.0.0)...\n");
    struct ip4_addr ip{}, nm{}, gw{};
    ip.addr = 0;  // 0.0.0.0 - will be assigned by DHCP
    nm.addr = 0;
    gw.addr = 0;
    res = ps2ipInit(&ip, &nm, &gw);
    if (res < 0)
    {
        printf("[PS2 Network] ps2ipInit failed (%d)\n", res);
        NetManDeinit();
        return false;
    }

    // 5. Enable DHCP on "sm0" (SMAP Ethernet device)
    char ifName[4] = "sm0";
    t_ip_info ipInfo{};
    std::strncpy(ipInfo.netif_name, ifName, sizeof(ipInfo.netif_name));
    if (ps2ip_getconfig(ifName, &ipInfo) >= 0)
    {
        ipInfo.dhcp_enabled = 1;
        ps2ip_setconfig(&ipInfo);
        printf("[PS2 Network] DHCP enabled on sm0\n");
    }

    s_initialized = true;

    // Wait for DHCP lease (up to 5 seconds in 100ms slices).
    // PCSX2 Sockets mode typically responds within ~500ms with 192.0.2.100.
    printf("[PS2 Network] Waiting for DHCP lease...\n");
    for (int i = 0; i < 50; ++i)
    {
        if (checkLinkState() && ps2ip_getconfig(ifName, &ipInfo) >= 0)
        {
            unsigned long rawIp = ipInfo.ipaddr.s_addr;
            // Accept any non-zero, non-link-local IP
            if (rawIp != 0 && (rawIp & 0xFFFF) != 0xFEA9)  // != 169.254.x.x
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                    static_cast<unsigned>(rawIp & 0xFF),
                    static_cast<unsigned>((rawIp >> 8) & 0xFF),
                    static_cast<unsigned>((rawIp >> 16) & 0xFF),
                    static_cast<unsigned>((rawIp >> 24) & 0xFF));
                s_ipAddress = buf;
                printf("[PS2 Network] DHCP bound! IP: %s (iteration %d)\n", s_ipAddress.c_str(), i);
                s_ready = true;
                return true;
            }
        }
        usleep(100000);  // 100ms
    }

    // If DHCP didn't respond, fall back to a static configuration.
    // This handles the case where PCSX2 InterceptDHCP is disabled.
    printf("[PS2 Network] DHCP timeout, falling back to static 192.0.2.100\n");
    t_ip_info staticInfo{};
    std::strncpy(staticInfo.netif_name, "sm0", sizeof(staticInfo.netif_name));
    staticInfo.ipaddr.s_addr = inet_addr("192.0.2.100");
    staticInfo.netmask.s_addr = inet_addr("255.255.255.0");
    staticInfo.gw.s_addr = inet_addr("192.0.2.1");
    staticInfo.dhcp_enabled = 0;
    ps2ip_setconfig(&staticInfo);
    s_ipAddress = "192.0.2.100";

    s_ready = true;
    printf("[PS2 Network] Network ready (static fallback). IP: %s\n", s_ipAddress.c_str());
    return true;
}

bool isReady()
{
    std::lock_guard<std::mutex> guard(s_stateMutex);
    return s_ready;
}

std::string getIpAddress()
{
    std::lock_guard<std::mutex> guard(s_stateMutex);
    return s_ipAddress;
}

DiagnosticResult testConnection()
{
    DiagnosticResult res;
    if (!initialize())
    {
        res.statusMessage = "Network hardware not detected (IRX failed)";
        return res;
    }
    res.hardwareOk = true;

    if (!checkLinkState())
    {
        res.statusMessage = "Link down (Ethernet cable disconnected)";
        return res;
    }
    res.linkUp = true;

    // Read our assigned IP
    char ifName[4] = "sm0";
    t_ip_info ipInfo{};
    if (ps2ip_getconfig(ifName, &ipInfo) >= 0)
    {
        unsigned long rawIp = ipInfo.ipaddr.s_addr;
        if (rawIp != 0 && (rawIp & 0xFFFF) != 0xFEA9)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                static_cast<unsigned>(rawIp & 0xFF),
                static_cast<unsigned>((rawIp >> 8) & 0xFF),
                static_cast<unsigned>((rawIp >> 16) & 0xFF),
                static_cast<unsigned>((rawIp >> 24) & 0xFF));
            res.ipAddress = buf;
            res.hasIp = true;
            std::lock_guard<std::mutex> guard(s_stateMutex);
            s_ipAddress = res.ipAddress;
            s_ready = true;
        }
    }

    if (!res.hasIp)
    {
        res.statusMessage = "Link up | No IP assigned (DHCP failed)";
        return res;
    }

    // Try TCP connect to the local Minecraft server at 192.168.0.52:25565
    // PCSX2 Sockets mode NATs from 192.0.2.x -> host PC's real network,
    // so connecting to 192.168.0.52 from the PS2 side goes through NAT.
    printf("[PS2 Network] testConnection: trying 192.168.0.52:25565...\n");
    bool localServerOk = false;
    int localMs = 0;
    const int lsock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsock >= 0)
    {
        int nb = 1;
        lwip_ioctl(lsock, FIONBIO, &nb);
        sockaddr_in ltarget{};
        ltarget.sin_len = sizeof(ltarget);
        ltarget.sin_family = AF_INET;
        ltarget.sin_port = htons(25565);
        ltarget.sin_addr.s_addr = inet_addr("192.168.0.52");

        auto lstart = std::chrono::steady_clock::now();
        int lconn = ::connect(lsock, reinterpret_cast<sockaddr *>(&ltarget), sizeof(ltarget));
        if (lconn < 0)
        {
            fd_set lws;
            FD_ZERO(&lws);
            FD_SET(lsock, &lws);
            struct timeval ltv{ 3, 0 }; // 3 second timeout
            if (::select(lsock + 1, nullptr, &lws, nullptr, &ltv) > 0)
            {
                int lerr = 0;
                socklen_t llen = sizeof(lerr);
                if (::getsockopt(lsock, SOL_SOCKET, SO_ERROR, &lerr, &llen) == 0 && lerr == 0)
                    localServerOk = true;
            }
        }
        else
        {
            localServerOk = true;
        }
        if (localServerOk)
        {
            auto lend = std::chrono::steady_clock::now();
            localMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(lend - lstart).count());
        }
        ::close(lsock);
    }

    if (localServerOk)
    {
        res.pingOk = true;
        res.pingMs = localMs;
        res.statusMessage = "Online! IP: " + res.ipAddress + " | Server (192.168.0.52:25565): " + std::to_string(localMs) + "ms";
    }
    else
    {
        // Try ping to 8.8.8.8:53
        printf("[PS2 Network] testConnection: trying 8.8.8.8:53...\n");
        const int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock >= 0)
        {
            int nb2 = 1;
            lwip_ioctl(sock, FIONBIO, &nb2);
            sockaddr_in target{};
            target.sin_len = sizeof(target);
            target.sin_family = AF_INET;
            target.sin_port = htons(53);
            target.sin_addr.s_addr = inet_addr("8.8.8.8");

            auto start = std::chrono::steady_clock::now();
            int conn = ::connect(sock, reinterpret_cast<sockaddr *>(&target), sizeof(target));
            if (conn < 0)
            {
                fd_set ws;
                FD_ZERO(&ws);
                FD_SET(sock, &ws);
                struct timeval tv{ 3, 0 };
                if (::select(sock + 1, nullptr, &ws, nullptr, &tv) > 0)
                {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                    {
                        auto end = std::chrono::steady_clock::now();
                        res.pingMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
                        res.pingOk = true;
                    }
                }
            }
            else
            {
                auto end = std::chrono::steady_clock::now();
                res.pingMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
                res.pingOk = true;
            }
            ::close(sock);
        }
        if (res.pingOk)
            res.statusMessage = "Online! IP: " + res.ipAddress + " | Ping 8.8.8.8: " + std::to_string(res.pingMs) + "ms | Local server unreachable";
        else
            res.statusMessage = "LAN OK: " + res.ipAddress + " | No internet/server detected";
    }

    return res;
}

void shutdown()
{
    std::lock_guard<std::mutex> guard(s_stateMutex);
    if (!s_initialized)
        return;

    ps2ipDeinit();
    NetManDeinit();
    s_initialized = false;
    s_ready = false;
    s_ipAddress.clear();
    MC_LOG_INFO("network", "[PS2] Network shutdown complete\n");
}

} // namespace Ps2Network

#endif // PS2_PLATFORM
