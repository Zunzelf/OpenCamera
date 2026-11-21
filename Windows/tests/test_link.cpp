// Headless PhoneLink test: auto-approves phones, prints phase changes and log.
#include "AppState.hpp"
#include "PhoneLink.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

static const char* PhaseName(Phase p)
{
	switch (p)
	{
	case Phase::Waiting: return "Waiting";
	case Phase::AskApproval: return "AskApproval";
	case Phase::Connecting: return "Connecting";
	case Phase::Live: return "Live";
	case Phase::Reconnecting: return "Reconnecting";
	case Phase::Error: return "Error";
	}
	return "?";
}

int main(int argc, char** argv)
{
	const int seconds = argc > 1 ? atoi(argv[1]) : 20;
	g_app.frontCamera = true;
	PhoneLink link;
	if (!link.start()) { printf("FAIL start\n"); return 1; }
	printf("READY\n");
	fflush(stdout);
	Phase last = (Phase)-1;
	bool front = true;
	size_t logged = 0;
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (std::chrono::steady_clock::now() < end)
	{
		if (g_app.phase == Phase::AskApproval && g_app.approvalAnswer == 0)
		{
			g_app.approvalRemember = false;
			g_app.approvalAnswer = 1;
		}
		if (g_app.phase.load() != last)
		{
			last = g_app.phase.load();
			std::string n, a;
			g_app.getPhone(n, a);
			printf("PHASE %s phone='%s' addr='%s'\n", PhaseName(last), n.c_str(), a.c_str());
		}
		if (g_app.frontCamera.load() != front)
		{
			front = g_app.frontCamera;
			printf("CAMERA %s\n", front ? "front" : "back");
		}
		auto lines = g_app.logSnapshot();
		for (; logged < lines.size(); logged++) printf("LOG %s\n", lines[logged].text.c_str());
		fflush(stdout);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	link.stop();
	printf("DONE\n");
	return 0;
}
