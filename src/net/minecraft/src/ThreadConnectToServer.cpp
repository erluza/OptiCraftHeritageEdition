#include "ThreadConnectToServer.h"

#include "platform/Log.h"
#include <exception>
#include <iostream>
#include <stdexcept>

#include "GuiConnecting.h"
#include "Minecraft.h"
#include "NetClientHandler.h"
#include "Packet2Handshake.h"
#include "Session.h"
#include "java/String.h"
#ifdef PS2_PLATFORM
#include <kernel.h>
#include "ps2/system/Ps2ThreadPriority.h"
#endif

ThreadConnectToServer::ThreadConnectToServer(GuiConnecting *guiconnecting, Minecraft *minecraft, const std::string &s, int_t i)
	: mc(minecraft)
	, hostName(s)
	, port(i)
{
	(void)guiconnecting;
}

ThreadConnectToServer::~ThreadConnectToServer()
{
	cancel();
	if (worker.joinable() && !worker.isCurrent())
		worker.join();
	std::lock_guard<std::mutex> guard(resultLock);
	if (resultHandler != nullptr)
	{
		resultHandler->disconnect();
		delete resultHandler;
		resultHandler = nullptr;
	}
}

void ThreadConnectToServer::start()
{
#ifdef PS2_PLATFORM
	if (!worker.start(&ThreadConnectToServer::wiiThreadEntry, this, 32 * 1024, Ps2ThreadPriority::kNetworkWorker))
		throw std::runtime_error("Could not create connection thread");
#else
	if (!worker.start(&ThreadConnectToServer::wiiThreadEntry, this, 32 * 1024, 64))
		throw std::runtime_error("Could not create connection thread");
#endif
}

void ThreadConnectToServer::cancel()
{
	cancelled.store(true);
}

NetClientHandler *ThreadConnectToServer::takeHandler()
{
	std::lock_guard<std::mutex> guard(resultLock);
	NetClientHandler *handler = resultHandler;
	resultHandler = nullptr;
	return handler;
}

bool ThreadConnectToServer::takeError(std::string &message)
{
	std::lock_guard<std::mutex> guard(resultLock);
	if (!errorPending)
		return false;
	message = resultError;
	errorPending = false;
	return true;
}

void *ThreadConnectToServer::wiiThreadEntry(void *argument)
{
	static_cast<ThreadConnectToServer *>(argument)->run();
	return nullptr;
}

void ThreadConnectToServer::run()
{
#ifdef PS2_PLATFORM
	ChangeThreadPriority(GetThreadId(), Ps2ThreadPriority::kNetworkWorker);
#endif
	printf("[PS2 Network] ThreadConnectToServer started for %s:%d\n", hostName.c_str(), port);
	try
	{
		NetClientHandler *handler = new NetClientHandler(mc, hostName, port);
		if (cancelled.load())
		{
			printf("[PS2 Network] Connection cancelled during connect\n");
			handler->disconnect();
			delete handler;
			return;
		}
		printf("[PS2 Network] Connection established, sending handshake for %s...\n", mc->session->username.c_str());
		handler->addToSendQueue(new Packet2Handshake(mc->session->username));
		std::lock_guard<std::mutex> guard(resultLock);
		resultHandler = handler;
		printf("[PS2 Network] Handshake queued, resultHandler assigned!\n");
	}
	catch (std::exception &exception)
	{
		if (cancelled.load())
			return;
		printf("[PS2 Network] ThreadConnectToServer exception: %s\n", exception.what());
		MC_LOG_ERROR("game", "%s\n", exception.what());
		std::lock_guard<std::mutex> guard(resultLock);
		resultError = exception.what();
		errorPending = true;
	}
}
