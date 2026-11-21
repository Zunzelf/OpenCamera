#pragma once
// Talks to the phone app.
//
// Control channel (TCP 65500, unchanged from the original protocol so the
// published Android app still works):
//   phone -> PC : 4 x int32 phone IPv4 octets, int32 RTSP port,
//                 [optional, new app] int32 'OCAM' magic, int32 len, UTF-8 name
//   phone -> PC : any bytes afterwards are heartbeats (old app: 2 bytes/s)
// Pro controls (phone app 3.1+, ignored by older apps):
//   PC -> phone : text lines "HELLO 2", "SET key=value ...", "POINT x y"
//   phone -> PC : 'M' + int32 length + text: "CAPS ...", "STAT ...", "EVT ..."
// Discovery (UDP 65501, new):
//   phone broadcasts "OPENCAMERA?"; PC answers "OPENCAMERA1|<pc name>|65500".
//   PC also broadcasts the same answer once a second.

#include <WinSock2.h>

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

class PhoneLink
{
public:
	static const int kControlPort = 65500;
	static const int kDiscoveryPort = 65501;

	PhoneLink();
	~PhoneLink();

	bool start();
	void stop();

	// Local IPv4 addresses, for the "type this address" fallback in the UI.
	static std::vector<std::string> localAddresses();
	static std::string computerName();

private:
	void controlLoop();
	void discoveryLoop();
	void videoLoop(std::string host, int port);
	bool handshake(SOCKET s, const std::string& peer, int& rtspPort, std::string& name);
	bool waitApproval(SOCKET s, const std::string& key, const std::string& label);
	void serveSession(SOCKET s, const std::string& host, int rtspPort);

	void loadTrusted();
	void saveTrusted();

	SOCKET m_listen = INVALID_SOCKET;
	SOCKET m_udp = INVALID_SOCKET;
	std::thread m_control;
	std::thread m_discovery;
	std::thread m_video;
	std::atomic<bool> m_running{ false };
	std::atomic<bool> m_stopVideo{ false };
	std::atomic<bool> m_videoDone{ true };
	std::set<std::string> m_trusted;
	std::set<std::string> m_blockedThisRun;
	bool m_wsa = false;
};
