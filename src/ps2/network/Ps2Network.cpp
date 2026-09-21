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

    // 1. Load IOP network modules
    int r_dev9 = Ps2IrxLoader::load("irx/ps2dev9.irx", "host:ps2dev9.irx");
    int r_netman = Ps2IrxLoader::load("irx/netman.irx", "host:netman.irx");
    int r_smap = Ps2IrxLoader::load("irx/smap.irx", "host:smap.irx");
    int r_ps2ip = Ps2IrxLoader::load("irx/ps2ip.irx", "host:ps2ip.irx");

    if (r_dev9 < 0 || r_netman < 0 || r_smap < 0 || r_ps2ip < 0)
    {
        MC_LOG_ERROR("network", "[PS2] Network modules failed to load (dev9=%d, netman=%d, smap=%d, ps2ip=%d)\n",
                     r_dev9, r_netman, r_smap, r_ps2ip);
        return false;
    }

    // 2. Initialize NETMAN service
    int res = NetManInit();
    if (res < 0)
    {
        MC_LOG_ERROR("network", "[PS2] NetManInit failed (%d)\n", res);
        return false;
    }

    // 3. Set Ethernet link mode to auto-negotiation
    applyLinkMode(NETMAN_NETIF_ETH_LINK_MODE_AUTO);

    // 4. Initialize TCP/IP stack with zeroes for DHCP
    struct ip4_addr ip{}, nm{}, gw{};
    res = ps2ipInit(&ip, &nm, &gw);
    if (res < 0)
    {
        MC_LOG_ERROR("network", "[PS2] ps2ipInit failed (%d)\n", res);
        NetManDeinit();
        return false;
    }

    // 5. Request DHCP on "sm0" (SMAP Ethernet device)
    char ifName[4] = "sm0";
    t_ip_info ipInfo{};
    if (ps2ip_getconfig(ifName, &ipInfo) >= 0)
    {
        ipInfo.dhcp_enabled = 1;
        ps2ip_setconfig(&ipInfo);
    }

    s_initialized = true;

    // Quick non-blocking probe for link and existing IP lease
    if (checkLinkState())
    {
        if (ps2ip_getconfig(ifName, &ipInfo) >= 0)
        {
            unsigned long rawIp = ipInfo.ipaddr.s_addr;
            if (rawIp != 0 && (ipInfo.dhcp_status == DHCP_STATE_BOUND || ipInfo.dhcp_status == DHCP_STATE_OFF))
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                    static_cast<unsigned>(rawIp & 0xFF),
                    static_cast<unsigned>((rawIp >> 8) & 0xFF),
                    static_cast<unsigned>((rawIp >> 16) & 0xFF),
                    static_cast<unsigned>((rawIp >> 24) & 0xFF));
                s_ipAddress = buf;
            }
        }
    }

    s_ready = true;
    MC_LOG_INFO("network", "[PS2] Network stack initialized\n");
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

    // Refresh IP info from SMAP interface; if DHCP is negotiating, wait briefly
    char ifName[4] = "sm0";
    t_ip_info ipInfo{};
    for (int i = 0; i < 15; ++i)
    {
        if (ps2ip_getconfig(ifName, &ipInfo) >= 0 && ipInfo.ipaddr.s_addr != 0)
            break;
        DelayThread(100000); // 100ms
    }

    if (ps2ip_getconfig(ifName, &ipInfo) >= 0)
    {
        unsigned long rawIp = ipInfo.ipaddr.s_addr;
        if (rawIp != 0)
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
        res.statusMessage = "Link up | Waiting for DHCP lease...";
        return res;
    }

    // Try TCP connect to 8.8.8.8:53 with a 1.2-second timeout
    const int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        res.statusMessage = "IP: " + res.ipAddress + " | Socket error";
        return res;
    }

    int nonblocking = 1;
    lwip_ioctl(sock, FIONBIO, &nonblocking);

    sockaddr_in target{};
    std::memset(&target, 0, sizeof(target));
    target.sin_len = sizeof(target);
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    target.sin_addr.s_addr = inet_addr("8.8.8.8");

    auto start = std::chrono::steady_clock::now();
    int conn = ::connect(sock, reinterpret_cast<sockaddr *>(&target), sizeof(target));
    if (conn < 0)
    {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);
        struct timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 200000;
        int sel = ::select(sock + 1, nullptr, &writeSet, nullptr, &tv);
        if (sel > 0)
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

    if (res.pingOk)
    {
        res.statusMessage = "Online! IP: " + res.ipAddress + " | Ping 8.8.8.8: " + std::to_string(res.pingMs) + "ms";
    }
    else
    {
        res.statusMessage = "LAN OK: " + res.ipAddress + " | Ping 8.8.8.8 timed out";
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
