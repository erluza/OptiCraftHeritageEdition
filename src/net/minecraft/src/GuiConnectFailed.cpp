#include "GuiConnectFailed.h"
#include "NetworkTelemetry.h"
#include "StringTranslate.h"
#include "GuiButton.h"
#include "GuiMainMenu.h"
#include "FontRenderer.h"
#include "Minecraft.h"
#include "platform/Input.h"

#ifdef PS2_PLATFORM
#include "ps2/input/Ps2PadState.h"
#endif

GuiConnectFailed::GuiConnectFailed(const std::string &errorKey, const std::string &detailKey, const std::string &arg)
{
	StringTranslate *stringtranslate = StringTranslate::getInstance();
	errorMessage = stringtranslate->translateKey(errorKey);
	if (!arg.empty())
		errorDetail = stringtranslate->translateKeyFormat(detailKey, arg.c_str());
	else
		errorDetail = stringtranslate->translateKey(detailKey);
}

void GuiConnectFailed::updateScreen()
{
}

void GuiConnectFailed::handleSpecializedMenuInput()
{
#ifdef PS2_PLATFORM
	const Ps2PadSnapshot &pad = ps2PadGetSnapshot(0);
	const bool selectPressed = (pad.pressed & PS2_PAD_SELECT) != 0;
	const bool comboPressed = ((pad.held & (PS2_PAD_L1 | PS2_PAD_R1)) == (PS2_PAD_L1 | PS2_PAD_R1)) &&
	                          ((pad.pressed & (PS2_PAD_L1 | PS2_PAD_R1)) != 0);
	if (selectPressed || comboPressed)
	{
		NetworkTelemetry::getInstance().toggleOverlay();
	}
#endif
	const PlatformTextInputSnapshot snap = platformTextInputSnapshot(platformMenuPad());
	if (snap.pressed & PLATFORM_TEXT_SPACE)
	{
		NetworkTelemetry::getInstance().toggleOverlay();
	}
}

void GuiConnectFailed::keyTyped(char_t c, int_t key)
{
	(void)c;
	if (key == 61 || key == 32 || key == 15)
	{
		NetworkTelemetry::getInstance().toggleOverlay();
	}
}

void GuiConnectFailed::initGui()
{
	StringTranslate *stringtranslate = StringTranslate::getInstance();
	controlList.clear();
	controlList.push_back(new GuiButton(0, width / 2 - 100, height / 4 + 120 + 12, stringtranslate->translateKey("gui.toMenu")));
}

void GuiConnectFailed::actionPerformed(GuiButton *guibutton)
{
	if (guibutton->id == 0)
	{
		mc->displayGuiScreen(new GuiMainMenu());
	}
}

void GuiConnectFailed::drawScreen(int_t i, int_t j, float_t f)
{
	drawDefaultBackground();
	drawCenteredString(fontRenderer, errorMessage, width / 2, height / 2 - 50, 0xffffff);
	drawCenteredString(fontRenderer, errorDetail,  width / 2, height / 2 - 10, 0xffffff);
	GuiScreen::drawScreen(i, j, f);

	drawCenteredString(fontRenderer, "[SELECT: Network Diagnostics]", width / 2, height - 12, 0x777777);

	if (NetworkTelemetry::getInstance().isOverlayVisible())
	{
		renderDebugOverlay();
	}
}

void GuiConnectFailed::renderDebugOverlay()
{
	NetworkTelemetry &telemetry = NetworkTelemetry::getInstance();

	const int boxX1 = 10;
	const int boxY1 = 8;
	const int boxX2 = width - 10;
	const int boxY2 = height - 20;

	drawRect(boxX1, boxY1, boxX2, boxY2, 0xd0080810);

	const int borderColor = 0xffff4444;
	drawRect(boxX1, boxY1, boxX2, boxY1 + 1, borderColor);
	drawRect(boxX1, boxY2 - 1, boxX2, boxY2, borderColor);
	drawRect(boxX1, boxY1, boxX1 + 1, boxY2, borderColor);
	drawRect(boxX2 - 1, boxY1, boxX2, boxY2, borderColor);

	int y = boxY1 + 5;
	const int x = boxX1 + 6;
	const int stepY = 10;

	char buf[128];
	std::snprintf(buf, sizeof(buf), "=== PS2 NETWORK TELEMETRY [%d ms] ===", telemetry.getElapsedMs());
	drawString(fontRenderer, buf, x, y, 0xffff55);
	y += stepY;

	std::snprintf(buf, sizeof(buf), "Stage: %s", telemetry.getStageName(telemetry.getStage()));
	drawString(fontRenderer, buf, x, y, 0xff5555);
	y += stepY;

	const std::string localIp = telemetry.getLocalIp();
	std::snprintf(buf, sizeof(buf), "Target: %s:%d | Local: %s",
	              telemetry.getTargetHost().c_str(),
	              telemetry.getTargetPort(),
	              localIp.empty() ? "none" : localIp.c_str());
	drawString(fontRenderer, buf, x, y, 0x55ffff);
	y += stepY;

	std::snprintf(buf, sizeof(buf), "FD: %d | Errno: %d | SO_ERR: %d | Sel: %d",
	              telemetry.getSocketFd(),
	              telemetry.getLastErrno(),
	              telemetry.getSoError(),
	              telemetry.getSelectResult());
	drawString(fontRenderer, buf, x, y, 0xffffff);
	y += stepY;

	std::snprintf(buf, sizeof(buf), "TX: %u B (%d pkts) | RX: %u B (%d pkts)",
	              static_cast<unsigned>(telemetry.getSentBytes()),
	              telemetry.getSentPackets(),
	              static_cast<unsigned>(telemetry.getReceivedBytes()),
	              telemetry.getReceivedPackets());
	drawString(fontRenderer, buf, x, y, 0xcccccc);
	y += stepY;

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

	drawString(fontRenderer, "--- Execution Log ---", x, y, 0x888888);
	y += stepY;

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
