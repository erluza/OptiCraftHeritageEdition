#include "java/JavaNetwork.h"

#ifdef PS2_PLATFORM

#include <atomic>
#include <cstring>
#include <cerrno>
#include <istream>
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include "ps2/network/Ps2Network.h"
#include "platform/Log.h"

extern "C"
{
int lwip_ioctl(int s, long cmd, void *argp);
}

namespace JavaNetwork
{
namespace
{

class Ps2Socket final : public Socket
{
public:
	~Ps2Socket() override
	{
		releaseSocket();
	}

	bool connect(const std::string &host, int port) override
	{
		releaseSocket();
		closing.store(false, std::memory_order_release);
		receivedBytes.store(0, std::memory_order_release);
		sentBytes.store(0, std::memory_order_release);
		remoteAddress = host + ":" + std::to_string(port);

		if (port < 1 || port > 65535 || !Ps2Network::initialize())
			return false;

		sockaddr_in target;
		std::memset(&target, 0, sizeof(target));
		target.sin_len = sizeof(target);
		target.sin_family = AF_INET;
		target.sin_port = htons(static_cast<unsigned short>(port));

		std::string resolvedHost = host;
		if (resolvedHost == "localhost" || resolvedHost == "127.0.0.1")
		{
			MC_LOG_INFO("network", "[PS2] Remapping localhost/127.0.0.1 to host PC IP (192.168.0.52)\n");
			resolvedHost = "192.168.0.52";
		}

		unsigned long ip = inet_addr(resolvedHost.c_str());
		if (ip != INADDR_NONE)
		{
			target.sin_addr.s_addr = ip;
		}
		else
		{
			hostent *resolved = gethostbyname(resolvedHost.c_str());
			if (resolved == nullptr || resolved->h_addr_list == nullptr || resolved->h_addr_list[0] == nullptr)
			{
				MC_LOG_WARN("network", "[PS2] Could not resolve host: %s\n", host.c_str());
				return false;
			}
			std::memcpy(&target.sin_addr, resolved->h_addr_list[0], sizeof(target.sin_addr));
		}

		printf("[PS2 Network] Connecting socket to %s (ip: %s, port: %d)...\n",
		       remoteAddress.c_str(), resolvedHost.c_str(), port);

		const int socketFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socketFd < 0)
		{
			printf("[PS2 Network] socket() failed: %d\n", socketFd);
			return false;
		}
		fd.store(socketFd, std::memory_order_release);

		int nodelay = 1;
		::setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

		// Use non-blocking connect with a 10-second timeout.
		// PS2 lwIP blocking connect has NO timeout, so an unreachable host
		// would hang the connection thread forever.
		int nb = 1;
		lwip_ioctl(socketFd, FIONBIO, &nb);

		int connRes = ::connect(socketFd, reinterpret_cast<sockaddr *>(&target), sizeof(target));
		if (connRes < 0 && errno != EINPROGRESS)
		{
			printf("[PS2 Network] connect() failed immediately: res=%d, errno=%d\n", connRes, errno);
			close();
			return false;
		}

		if (connRes != 0)
		{
			// Wait for connection to complete or timeout
			fd_set writeSet;
			FD_ZERO(&writeSet);
			FD_SET(socketFd, &writeSet);
			struct timeval tv{};
			tv.tv_sec = 10;
			tv.tv_usec = 0;

			printf("[PS2 Network] Waiting for connect (up to 10s)...\n");
			int sel = ::select(socketFd + 1, nullptr, &writeSet, nullptr, &tv);
			if (sel <= 0)
			{
				printf("[PS2 Network] connect() timed out (select=%d, errno=%d)\n", sel, errno);
				close();
				return false;
			}

			// Check if the connection succeeded
			int sockErr = 0;
			socklen_t errLen = sizeof(sockErr);
			if (::getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &sockErr, &errLen) != 0 || sockErr != 0)
			{
				printf("[PS2 Network] connect() failed: SO_ERROR=%d\n", sockErr);
				close();
				return false;
			}
		}

		// Switch back to blocking mode for read/write operations
		nb = 0;
		lwip_ioctl(socketFd, FIONBIO, &nb);

		printf("[PS2 Network] Connected successfully to %s!\n", remoteAddress.c_str());
		return true;
	}

	int read(char *buffer, int length) override
	{
		const int socketFd = fd.load(std::memory_order_acquire);
		if (socketFd < 0 || buffer == nullptr || length <= 0 ||
		    closing.load(std::memory_order_acquire))
			return -1;

		while (!closing.load(std::memory_order_acquire))
		{
			const int count = ::recv(socketFd, buffer, length, 0);
			if (count > 0)
			{
				receivedBytes.fetch_add(static_cast<std::size_t>(count), std::memory_order_relaxed);
				return count;
			}
			if (count == 0)
			{
				printf("[PS2 Network] recv returned 0: connection closed by server\n");
				return 0;
			}
			if (errno == EINTR)
			{
				continue;
			}
			printf("[PS2 Network] recv() error: errno=%d\n", errno);
			return -1;
		}
		return -1;
	}

	bool write(const char *buffer, int length) override
	{
		const int socketFd = fd.load(std::memory_order_acquire);
		if (socketFd < 0 || buffer == nullptr || closing.load(std::memory_order_acquire))
			return false;

		int offset = 0;
		while (offset < length)
		{
			if (closing.load(std::memory_order_acquire))
				return false;

			const int count = ::send(socketFd, buffer + offset, length - offset, 0);
			if (count > 0)
			{
				sentBytes.fetch_add(static_cast<std::size_t>(count), std::memory_order_relaxed);
				offset += count;
				continue;
			}
			if (count < 0 && errno == EINTR)
			{
				continue;
			}
			printf("[PS2 Network] send() error: errno=%d\n", errno);
			return false;
		}
		return true;
	}

	bool flush() override
	{
		return fd.load(std::memory_order_acquire) >= 0 &&
		       !closing.load(std::memory_order_acquire);
	}

	void interruptRead() override
	{
		const int socketFd = fd.load(std::memory_order_acquire);
		if (socketFd >= 0)
			::shutdown(socketFd, SHUT_RD);
	}

	void close() override
	{
		closing.store(true, std::memory_order_release);
		const int socketFd = fd.load(std::memory_order_acquire);
		if (socketFd >= 0)
			::shutdown(socketFd, SHUT_RDWR);
	}

	std::string getRemoteSocketAddress() const override
	{
		return remoteAddress;
	}

	std::size_t getReceivedByteCount() const override
	{
		return receivedBytes.load(std::memory_order_relaxed);
	}

	std::size_t getSentByteCount() const override
	{
		return sentBytes.load(std::memory_order_relaxed);
	}

private:
	void releaseSocket()
	{
		closing.store(true, std::memory_order_release);
		const int socketFd = fd.exchange(-1, std::memory_order_acq_rel);
		if (socketFd >= 0)
		{
			::shutdown(socketFd, SHUT_RDWR);
			::close(socketFd);
		}
	}

	std::atomic<int> fd{-1};
	std::atomic_bool closing{true};
	std::atomic<std::size_t> receivedBytes{0};
	std::atomic<std::size_t> sentBytes{0};
	std::string remoteAddress;
};

class SocketInputBuffer final : public std::streambuf
{
public:
	explicit SocketInputBuffer(Socket &value) : socket(value)
	{
		setg(buffer, buffer, buffer);
	}

protected:
	int_type underflow() override
	{
		if (gptr() < egptr())
			return traits_type::to_int_type(*gptr());

		int count = socket.read(buffer, sizeof(buffer));
		if (count <= 0)
			return traits_type::eof();

		setg(buffer, buffer, buffer + count);
		return traits_type::to_int_type(*gptr());
	}

private:
	Socket &socket;
	char buffer[2048];
};

class SocketOutputBuffer final : public std::streambuf
{
public:
	explicit SocketOutputBuffer(Socket &value) : socket(value)
	{
		setp(buffer, buffer + sizeof(buffer));
	}

	~SocketOutputBuffer() override
	{
		sync();
	}

protected:
	std::streamsize xsputn(const char *data, std::streamsize length) override
	{
		std::streamsize written = 0;
		while (written < length)
		{
			std::streamsize space = epptr() - pptr();
			if (space == 0)
			{
				if (!flushBuffer())
					return written;
				space = epptr() - pptr();
			}

			const std::streamsize remaining = length - written;
			const std::streamsize count = remaining < space ? remaining : space;
			std::memcpy(pptr(), data + written, static_cast<std::size_t>(count));
			pbump(static_cast<int>(count));
			written += count;
		}
		return written;
	}

	int_type overflow(int_type ch) override
	{
		if (traits_type::eq_int_type(ch, traits_type::eof()))
			return traits_type::not_eof(ch);

		if (!flushBuffer())
			return traits_type::eof();

		*pptr() = traits_type::to_char_type(ch);
		pbump(1);
		return traits_type::not_eof(ch);
	}

	int sync() override
	{
		return flushBuffer() && socket.flush() ? 0 : -1;
	}

private:
	bool flushBuffer()
	{
		const std::ptrdiff_t count = pptr() - pbase();
		if (count > 0 && !socket.write(pbase(), static_cast<int>(count)))
			return false;

		setp(buffer, buffer + sizeof(buffer));
		return true;
	}

	Socket &socket;
	char buffer[5120];
};

class SocketInputStream final : public std::istream
{
public:
	explicit SocketInputStream(Socket &socket)
		: std::istream(nullptr), buffer(socket)
	{
		rdbuf(&buffer);
	}

private:
	SocketInputBuffer buffer;
};

class SocketOutputStream final : public std::ostream
{
public:
	explicit SocketOutputStream(Socket &socket)
		: std::ostream(nullptr), buffer(socket)
	{
		rdbuf(&buffer);
	}

private:
	SocketOutputBuffer buffer;
};

} // namespace

std::unique_ptr<Socket> createSocket()
{
	return std::make_unique<Ps2Socket>();
}

std::unique_ptr<std::istream> createInputStream(Socket &socket)
{
	return std::make_unique<SocketInputStream>(socket);
}

std::unique_ptr<std::ostream> createOutputStream(Socket &socket)
{
	return std::make_unique<SocketOutputStream>(socket);
}

bool readUrl(const std::string &, std::vector<unsigned char> &)
{
	return false;
}

int getResponseCode(const std::string &)
{
	return 404;
}

bool postUrl(const std::string &, const std::string &, const std::string &,
             std::vector<unsigned char> &)
{
	return false;
}

} // namespace JavaNetwork

#endif // PS2_PLATFORM
