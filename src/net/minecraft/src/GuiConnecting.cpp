#include "platform/Log.h"
#include "GuiConnecting.h"
#include "NetworkTelemetry.h"
#include "ThreadConnectToServer.h"
#include "NetClientHandler.h"
#include "StringTranslate.h"
#include "GuiButton.h"
#include "GuiMainMenu.h"
#include "GuiConnectFailed.h"
#include "FontRenderer.h"
#include "Minecraft.h"
#include "platform/Input.h"

#include <cstdio>
#include <iostream>

#ifdef PS2_PLATFORM
#include <kernel.h>
#include <unistd.h>
#include "ps2/system/Ps2ThreadPriority.h"
#include "ps2/input/Ps2PadState.h"
#include "ps2/network/Ps2Network.h"
#endif

GuiConnecting::GuiConnecting(Minecraft *minecraft, const std::string &host, int_t port)
	: clientHandler(nullptr)
	, connectThread(nullptr)
	, cancelled(false)
	, ticksOpen(0)
{
	MC_LOG_INFO("network", "Connecting to %s, %d\n", host.c_str(), port);

	NetworkTelemetry &telemetry = NetworkTelemetry::getInstance();
	telemetry.reset();
	telemetry.setTarget(host, port);
#ifdef PS2_PLATFORM
	telemetry.setLocalIp(Ps2Network::getIpAddress());
#endif
	telemetry.startTimer();
	telemetry.setStage(ConnectStage::VALIDATING_ADDRESS, "Initiating connection to target host");

	minecraft->changeWorld1(nullptr);
	connectThread = new ThreadConnectToServer(this, minecraft, host, port);
	connectThread->start();
}

GuiConnecting::~GuiConnecting()
{
	if (connectThread != nullptr)
	{
		connectThread->cancel();
		delete connectThread;
		connectThread = nullptr;
	}
	if (clientHandler != nullptr)
	{
		delete clientHandler;
		clientHandler = nullptr;
	}
}

void GuiConnecting::handleSpecializedMenuInput()
{
#ifdef PS2_PLATFORM
	const Ps2PadSnapshot &pad = ps2PadGetSnapshot(0);
	// Single SELECT button press
	const bool selectPressed = (pad.pressed & PS2_PAD_SELECT) != 0;
	// L1 + R1 combo press
	const bool comboPressed = ((pad.held & (PS2_PAD_L1 | PS2_PAD_R1)) == (PS2_PAD_L1 | PS2_PAD_R1)) &&
	                          ((pad.pressed & (PS2_PAD_L1 | PS2_PAD_R1)) != 0);
	if (selectPressed || comboPressed)
	{
		NetworkTelemetry::getInstance().toggleOverlay();
	}
#endif
	const PlatformTextInputSnapshot snap = platformTextInputSnapshot(platformMenuPad());
	if (snap.pressed & PLATFORM_TEXT_SPACE) // SELECT maps to PLATFORM_TEXT_SPACE in platform backend
	{
		NetworkTelemetry::getInstance().toggleOverlay();
	}
}

void GuiConnecting::updateScreen()
{
	ticksOpen++;
	if (ticksOpen >= 30 && !controlList.empty() && controlList[0] != nullptr && !controlList[0]->enabled)
	{
		controlList[0]->enabled = true;
	}

#ifdef PS2_PLATFORM
	RotateThreadReadyQueue(Ps2ThreadPriority::kNetworkWorker);
	RotateThreadReadyQueue(Ps2ThreadPriority::kNetworkReader);
	RotateThreadReadyQueue(Ps2ThreadPriority::kNetworkWriter);
	RotateThreadReadyQueue(86);
	RotateThreadReadyQueue(87);
	RotateThreadReadyQueue(89);
	RotateThreadReadyQueue(Ps2ThreadPriority::kMain);
	usleep(1000);
#endif
	if (clientHandler == nullptr && connectThread != nullptr)
	{
		clientHandler = connectThread->takeHandler();
		if (clientHandler != nullptr)
		{
			NetworkTelemetry::getInstance().setStage(ConnectStage::LOGGING_IN, "GuiConnecting acquired clientHandler");
		}
	}

	std::string connectionError;
	if (connectThread != nullptr && connectThread->takeError(connectionError))
	{
		NetworkTelemetry::getInstance().setError(connectionError);
		mc->displayGuiScreen(new GuiConnectFailed(
			"connect.failed", "disconnect.genericReason", connectionError));
		return;
	}

	if (clientHandler != nullptr)
	{
		clientHandler->processReadPackets();
		if (clientHandler->isOwnedByPlayerController())
			clientHandler = nullptr;
	}
}

void GuiConnecting::keyTyped(char_t c, int_t key)
{
	(void)c;
	// Keyboard shortcut for debug HUD: F3 (61), D (32), Tab (15)
	if (key == 61 || key == 32 || key == 15)
	{
		NetworkTelemetry::getInstance().toggleOverlay();
	}
}

void GuiConnecting::initGui()
{
	StringTranslate *stringtranslate = StringTranslate::getInstance();
	controlList.clear();
	GuiButton *cancelBtn = new GuiButton(0, width / 2 - 100, height / 4 + 120 + 12, stringtranslate->translateKey("gui.cancel"));
	cancelBtn->enabled = (ticksOpen >= 30);
	controlList.push_back(cancelBtn);
}

void GuiConnecting::actionPerformed(GuiButton *guibutton)
{
	if (guibutton->id == 0)
	{
		// Debounce: prevent accidental cancel if button was pressed during screen transition
		if (ticksOpen < 30)
		{
			NetworkTelemetry::getInstance().logEvent("Cancel ignored (debounced, ticksOpen=%d)", (int)ticksOpen);
			return;
		}

		NetworkTelemetry::getInstance().logEvent("User cancelled connection");
		cancelled = true;
		if (connectThread != nullptr)
			connectThread->cancel();
		if (clientHandler != nullptr)
		{
			clientHandler->disconnect();
		}
		mc->displayGuiScreen(new GuiMainMenu());
	}
}

void GuiConnecting::drawScreen(int_t i, int_t j, float_t f)
{
	drawDefaultBackground();
	StringTranslate *stringtranslate = StringTranslate::getInstance();
	if (clientHandler == nullptr)
	{
		drawCenteredString(fontRenderer, stringtranslate->translateKey("connect.connecting"),   width / 2, height / 2 - 50, 0xffffff);
		drawCenteredString(fontRenderer, "",                                                    width / 2, height / 2 - 10, 0xffffff);
	}
	else
	{
		drawCenteredString(fontRenderer, stringtranslate->translateKey("connect.authorizing"),  width / 2, height / 2 - 50, 0xffffff);
		drawCenteredString(fontRenderer, clientHandler->getServerHostname(),                   width / 2, height / 2 - 10, 0xffffff);
	}

	GuiScreen::drawScreen(i, j, f);

	// Hint displayed at bottom so user knows they can view diagnostics anytime
	drawCenteredString(fontRenderer, "[SELECT: Network Diagnostics]", width / 2, height - 12, 0x777777);

	if (NetworkTelemetry::getInstance().isOverlayVisible())
	{
		renderDebugOverlay();
	}
}

void GuiConnecting::renderDebugOverlay()
{
	NetworkTelemetry &telemetry = NetworkTelemetry::getInstance();

	const int boxX1 = 10;
	const int boxY1 = 8;
	const int boxX2 = width - 10;
	const int boxY2 = height - 20;

	// Background: dark translucent box
	drawRect(boxX1, boxY1, boxX2, boxY2, 0xd0080810);

	// Outline border (green if connected/running, red if failed)
	const int borderColor = (telemetry.getStage() == ConnectStage::FAILED) ? 0xffff4444 : 0xff00ffcc;
	drawRect(boxX1, boxY1, boxX2, boxY1 + 1, borderColor);
	drawRect(boxX1, boxY2 - 1, boxX2, boxY2, borderColor);
	drawRect(boxX1, boxY1, boxX1 + 1, boxY2, borderColor);
	drawRect(boxX2 - 1, boxY1, boxX2, boxY2, borderColor);

	int y = boxY1 + 5;
	const int x = boxX1 + 6;
	const int stepY = 10;

	// Header
	char buf[128];
	std::snprintf(buf, sizeof(buf), "=== PS2 NETWORK TELEMETRY [%d ms] ===", telemetry.getElapsedMs());
	drawString(fontRenderer, buf, x, y, 0xffff55);
	y += stepY;

	// Stage
	std::snprintf(buf, sizeof(buf), "Stage: %s", telemetry.getStageName(telemetry.getStage()));
	const int stageColor = (telemetry.getStage() == ConnectStage::FAILED) ? 0xff5555 : 0x55ff55;
	drawString(fontRenderer, buf, x, y, stageColor);
	y += stepY;

	// Target & Local
	const std::string localIp = telemetry.getLocalIp();
	std::snprintf(buf, sizeof(buf), "Target: %s:%d | Local: %s",
	              telemetry.getTargetHost().c_str(),
	              telemetry.getTargetPort(),
	              localIp.empty() ? "none" : localIp.c_str());
	drawString(fontRenderer, buf, x, y, 0x55ffff);
	y += stepY;

	// Socket info
	std::snprintf(buf, sizeof(buf), "FD: %d | Errno: %d | SO_ERR: %d | Sel: %d",
	              telemetry.getSocketFd(),
	              telemetry.getLastErrno(),
	              telemetry.getSoError(),
	              telemetry.getSelectResult());
	drawString(fontRenderer, buf, x, y, 0xffffff);
	y += stepY;

	// Traffic
	std::snprintf(buf, sizeof(buf), "TX: %u B (%d pkts) | RX: %u B (%d pkts)",
	              static_cast<unsigned>(telemetry.getSentBytes()),
	              telemetry.getSentPackets(),
	              static_cast<unsigned>(telemetry.getReceivedBytes()),
	              telemetry.getReceivedPackets());
	drawString(fontRenderer, buf, x, y, 0xcccccc);
	y += stepY;

	// Threads
	auto threadStr = [](int state) -> const char* {
		if (state == 1) return "RUN";
		if (state == 2) return "DONE";
		if (state == -1) return "ERR";
		return "IDLE";
	};
	std::snprintf(buf, sizeof(buf), "Threads: Worker=%s Reader=%s Writer=%s",
	              threadStr(telemetry.getWorkerThreadState()),
	              threadStr(telemetry.getReaderThreadState()),
	              threadStr(telemetry.getWriterThreadState()));
	drawString(fontRenderer, buf, x, y, 0xffffaa);
	y += stepY;

	// Divider
	drawString(fontRenderer, "--- Execution Log ---", x, y, 0x888888);
	y += stepY;

	// Recent logs (up to 5 lines)
	std::vector<std::string> logs = telemetry.getRecentLogs(5);
	for (const auto &log : logs)
	{
		if (y + stepY > boxY2 - 2)
			break;
		const int logColor = (log.find("ERR") != std::string::npos ||
		                      log.find("fail") != std::string::npos ||
		                      log.find("timeout") != std::string::npos) ? 0xff7777 : 0xbbbbbb;
		drawString(fontRenderer, log, x, y, logColor);
		y += stepY;
	}
}

void GuiConnecting::setNetClientHandler(GuiConnecting *guiconnecting, NetClientHandler *netclienthandler)
{
	guiconnecting->clientHandler = netclienthandler;
}

NetClientHandler *GuiConnecting::getNetClientHandler(GuiConnecting *guiconnecting)
{
	return guiconnecting->clientHandler;
}

bool GuiConnecting::isCancelled(GuiConnecting *guiconnecting)
{
	return guiconnecting->cancelled;
}
