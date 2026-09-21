#include "NetworkTelemetry.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>

NetworkTelemetry::NetworkTelemetry()
{
}

NetworkTelemetry &NetworkTelemetry::getInstance()
{
    static NetworkTelemetry instance;
    return instance;
}

void NetworkTelemetry::reset()
{
    currentStage.store(ConnectStage::IDLE, std::memory_order_relaxed);
    socketFd.store(-1, std::memory_order_relaxed);
    lastErrno.store(0, std::memory_order_relaxed);
    soError.store(0, std::memory_order_relaxed);
    selectResult.store(0, std::memory_order_relaxed);
    sentBytes.store(0, std::memory_order_relaxed);
    receivedBytes.store(0, std::memory_order_relaxed);
    sentPackets.store(0, std::memory_order_relaxed);
    receivedPackets.store(0, std::memory_order_relaxed);
    workerState.store(0, std::memory_order_relaxed);
    readerState.store(0, std::memory_order_relaxed);
    writerState.store(0, std::memory_order_relaxed);
    // Note: overlayVisible is preserved across resets so user's toggle preference persists!
    timerStarted.store(false, std::memory_order_relaxed);

    std::lock_guard<std::mutex> guard(dataMutex);
    targetHost.clear();
    targetPort = 25565;
    lastDetail.clear();
    errorMessage.clear();
    recentLogs.clear();
}

void NetworkTelemetry::setStage(ConnectStage stage, const char *detail)
{
    currentStage.store(stage, std::memory_order_relaxed);
    if (detail != nullptr && detail[0] != '\0')
    {
        logEvent("[Stage %s] %s", getStageName(stage), detail);
    }
    else
    {
        logEvent("[Stage %s]", getStageName(stage));
    }
}

const char *NetworkTelemetry::getStageName(ConnectStage stage) const
{
    switch (stage)
    {
    case ConnectStage::IDLE:                 return "Idle";
    case ConnectStage::VALIDATING_ADDRESS:   return "1/7 Validating IP / DNS";
    case ConnectStage::CREATING_SOCKET:      return "2/7 Creating Socket";
    case ConnectStage::TCP_CONNECTING:       return "3/7 TCP Connect (select)";
    case ConnectStage::INITIALIZING_STREAMS: return "4/7 Streams & Threads";
    case ConnectStage::SENDING_HANDSHAKE:    return "5/7 Handshake (Packet 2)";
    case ConnectStage::LOGGING_IN:           return "6/7 Authorizing (Packet 1)";
    case ConnectStage::CONNECTED:            return "7/7 Connected (In-Game!)";
    case ConnectStage::FAILED:               return "FAILED";
    default:                                 return "Unknown";
    }
}

int NetworkTelemetry::getStageNumber(ConnectStage stage) const
{
    return static_cast<int>(stage);
}

void NetworkTelemetry::setTarget(const std::string &host, int port)
{
    std::lock_guard<std::mutex> guard(dataMutex);
    targetHost = host;
    targetPort = port;
}

std::string NetworkTelemetry::getTargetHost() const
{
    std::lock_guard<std::mutex> guard(dataMutex);
    return targetHost;
}

int NetworkTelemetry::getTargetPort() const
{
    std::lock_guard<std::mutex> guard(dataMutex);
    return targetPort;
}

void NetworkTelemetry::setLocalIp(const std::string &ip)
{
    std::lock_guard<std::mutex> guard(dataMutex);
    localIp = ip;
}

std::string NetworkTelemetry::getLocalIp() const
{
    std::lock_guard<std::mutex> guard(dataMutex);
    return localIp;
}

void NetworkTelemetry::setSocketFd(int fd)
{
    socketFd.store(fd, std::memory_order_relaxed);
}

void NetworkTelemetry::setLastErrno(int err)
{
    lastErrno.store(err, std::memory_order_relaxed);
}

void NetworkTelemetry::setSoError(int err)
{
    soError.store(err, std::memory_order_relaxed);
}

void NetworkTelemetry::setSelectResult(int sel)
{
    selectResult.store(sel, std::memory_order_relaxed);
}

void NetworkTelemetry::addSentBytes(std::size_t bytes)
{
    sentBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NetworkTelemetry::addReceivedBytes(std::size_t bytes)
{
    receivedBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NetworkTelemetry::addSentPacket()
{
    sentPackets.fetch_add(1, std::memory_order_relaxed);
}

void NetworkTelemetry::addReceivedPacket()
{
    receivedPackets.fetch_add(1, std::memory_order_relaxed);
}

void NetworkTelemetry::setWorkerThreadState(int state)
{
    workerState.store(state, std::memory_order_relaxed);
}

void NetworkTelemetry::setReaderThreadState(int state)
{
    readerState.store(state, std::memory_order_relaxed);
}

void NetworkTelemetry::setWriterThreadState(int state)
{
    writerState.store(state, std::memory_order_relaxed);
}

void NetworkTelemetry::startTimer()
{
    startTime = std::chrono::steady_clock::now();
    timerStarted.store(true, std::memory_order_relaxed);
}

int NetworkTelemetry::getElapsedMs() const
{
    if (!timerStarted.load(std::memory_order_relaxed))
        return 0;
    auto now = std::chrono::steady_clock::now();
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
}

void NetworkTelemetry::logEvent(const char *fmt, ...)
{
    char buf[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    int ms = getElapsedMs();
    char formatted[512];
    snprintf(formatted, sizeof(formatted), "[+%4dms] %s", ms, buf);

    // 1. Unbuffered stdout print
    printf("[PS2 Network] %s\n", formatted);
    fflush(stdout);

    // 2. Direct PCSX2 HostFS file logging
    FILE *hostFile = fopen("host:debug_network.log", "a");
    if (hostFile != nullptr)
    {
        fprintf(hostFile, "%s\n", formatted);
        fflush(hostFile);
        fclose(hostFile);
    }

    // 3. Direct local file logging
    FILE *localFile = fopen("debug_network.log", "a");
    if (localFile != nullptr)
    {
        fprintf(localFile, "%s\n", formatted);
        fflush(localFile);
        fclose(localFile);
    }

    // 4. In-memory ring buffer for on-screen HUD
    {
        std::lock_guard<std::mutex> guard(dataMutex);
        recentLogs.push_back(formatted);
        while (recentLogs.size() > 10)
            recentLogs.pop_front();
        lastDetail = buf;
    }
}

std::vector<std::string> NetworkTelemetry::getRecentLogs(std::size_t maxCount) const
{
    std::lock_guard<std::mutex> guard(dataMutex);
    std::vector<std::string> result;
    std::size_t count = recentLogs.size();
    std::size_t start = count > maxCount ? count - maxCount : 0;
    for (std::size_t i = start; i < count; ++i)
    {
        result.push_back(recentLogs[i]);
    }
    return result;
}

std::string NetworkTelemetry::getLastDetail() const
{
    std::lock_guard<std::mutex> guard(dataMutex);
    return lastDetail;
}

std::string NetworkTelemetry::getErrorMessage() const
{
    std::lock_guard<std::mutex> guard(dataMutex);
    return errorMessage;
}

void NetworkTelemetry::setError(const std::string &err)
{
    {
        std::lock_guard<std::mutex> guard(dataMutex);
        errorMessage = err;
    }
    setStage(ConnectStage::FAILED, err.c_str());
}
