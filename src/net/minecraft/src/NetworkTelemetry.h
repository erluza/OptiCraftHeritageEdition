#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>

enum class ConnectStage : int
{
    IDLE = 0,
    VALIDATING_ADDRESS = 1,   // Checking IP format / DNS resolution
    CREATING_SOCKET = 2,      // Calling socket(AF_INET, SOCK_STREAM, 0)
    TCP_CONNECTING = 3,       // connect() + select() timeout polling
    INITIALIZING_STREAMS = 4, // Input/output streams & thread startup
    SENDING_HANDSHAKE = 5,    // Packet2Handshake queued & sent
    LOGGING_IN = 6,           // Handshake accepted, Packet1Login exchanging
    CONNECTED = 7,            // Logged in! Loading world
    FAILED = 8                // Connection failed / error
};

class NetworkTelemetry
{
public:
    static NetworkTelemetry &getInstance();

    void reset();

    // Stage management
    void setStage(ConnectStage stage, const char *detail = nullptr);
    ConnectStage getStage() const { return currentStage.load(std::memory_order_relaxed); }
    const char *getStageName(ConnectStage stage) const;
    int getStageNumber(ConnectStage stage) const;

    // Target info
    void setTarget(const std::string &host, int port);
    std::string getTargetHost() const;
    int getTargetPort() const;

    // Local IP
    void setLocalIp(const std::string &ip);
    std::string getLocalIp() const;

    // Socket info
    void setSocketFd(int fd);
    int getSocketFd() const { return socketFd.load(std::memory_order_relaxed); }

    void setLastErrno(int err);
    int getLastErrno() const { return lastErrno.load(std::memory_order_relaxed); }

    void setSoError(int err);
    int getSoError() const { return soError.load(std::memory_order_relaxed); }

    void setSelectResult(int sel);
    int getSelectResult() const { return selectResult.load(std::memory_order_relaxed); }

    // Traffic statistics
    void addSentBytes(std::size_t bytes);
    void addReceivedBytes(std::size_t bytes);
    std::size_t getSentBytes() const { return sentBytes.load(std::memory_order_relaxed); }
    std::size_t getReceivedBytes() const { return receivedBytes.load(std::memory_order_relaxed); }

    void addSentPacket();
    void addReceivedPacket();
    int getSentPackets() const { return sentPackets.load(std::memory_order_relaxed); }
    int getReceivedPackets() const { return receivedPackets.load(std::memory_order_relaxed); }

    // Thread states
    // 0 = not started, 1 = running, 2 = finished/exited, -1 = error
    void setWorkerThreadState(int state);
    int getWorkerThreadState() const { return workerState.load(std::memory_order_relaxed); }

    void setReaderThreadState(int state);
    int getReaderThreadState() const { return readerState.load(std::memory_order_relaxed); }

    void setWriterThreadState(int state);
    int getWriterThreadState() const { return writerState.load(std::memory_order_relaxed); }

    // Elapsed time
    void startTimer();
    int getElapsedMs() const;

    // Logging & detail message
    void logEvent(const char *fmt, ...);
    std::vector<std::string> getRecentLogs(std::size_t maxCount = 6) const;
    std::string getLastDetail() const;
    std::string getErrorMessage() const;
    void setError(const std::string &err);

    // Visual overlay visibility toggle
    bool isOverlayVisible() const { return overlayVisible.load(std::memory_order_relaxed); }
    void setOverlayVisible(bool visible) { overlayVisible.store(visible, std::memory_order_relaxed); }
    void toggleOverlay() { overlayVisible.store(!overlayVisible.load(std::memory_order_relaxed), std::memory_order_relaxed); }

private:
    NetworkTelemetry();

    std::atomic<ConnectStage> currentStage{ConnectStage::IDLE};
    std::atomic<int> socketFd{-1};
    std::atomic<int> lastErrno{0};
    std::atomic<int> soError{0};
    std::atomic<int> selectResult{0};
    std::atomic<std::size_t> sentBytes{0};
    std::atomic<std::size_t> receivedBytes{0};
    std::atomic<int> sentPackets{0};
    std::atomic<int> receivedPackets{0};
    std::atomic<int> workerState{0};
    std::atomic<int> readerState{0};
    std::atomic<int> writerState{0};
    std::atomic<bool> overlayVisible{false}; // Hidden by default

    std::chrono::steady_clock::time_point startTime;
    std::atomic<bool> timerStarted{false};

    mutable std::mutex dataMutex;
    std::string targetHost;
    int targetPort = 25565;
    std::string localIp;
    std::string lastDetail;
    std::string errorMessage;
    std::deque<std::string> recentLogs;
};
