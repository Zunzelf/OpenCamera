#include "PhoneLink.hpp"
#include "AppState.hpp"
#include "VideoReceiver.hpp"
#include "../shared/FrameShm.h"

#include <WS2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

ocam::FrameWriter g_frames; // also read by the UI preview

static const uint32_t kNameMagic = 0x4F43414Du; // 'OCAM'

// Tests build with OPENCAMERA_LOOPBACK_TEST so they never listen on the LAN
// (and never trigger a firewall prompt).
#ifdef OPENCAMERA_LOOPBACK_TEST
static const ULONG kBindAddr = INADDR_LOOPBACK;
#else
static const ULONG kBindAddr = INADDR_ANY; // every adapter, no internet needed
#endif

static std::string AddrToString(const sockaddr_in& a)
{
	char buf[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
	return buf;
}

// Wait until `s` is readable or `ms` passes. Returns >0 readable, 0 timeout, <0 error.
static int WaitReadable(SOCKET s, int ms)
{
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(s, &fds);
	timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	return select(0, &fds, nullptr, nullptr, &tv);
}

// Read exactly n bytes within timeoutMs total, giving up early on stop.
static bool RecvExact(SOCKET s, char* buf, int n, int timeoutMs, const std::atomic<bool>& running)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	int got = 0;
	while (got < n && running)
	{
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
		if (left <= 0) return false;
		const int w = WaitReadable(s, (int)std::min<long long>(left, 200));
		if (w < 0) return false;
		if (w == 0) continue;
		const int r = recv(s, buf + got, n - got, 0);
		if (r <= 0) return false;
		got += r;
	}
	return got == n;
}

static bool RecvInt(SOCKET s, int32_t& v, int timeoutMs, const std::atomic<bool>& running)
{
	uint32_t be = 0;
	if (!RecvExact(s, (char*)&be, 4, timeoutMs, running)) return false;
	v = (int32_t)ntohl(be);
	return true;
}

PhoneLink::PhoneLink()
{
	WSADATA data{};
	m_wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

PhoneLink::~PhoneLink()
{
	stop();
	if (m_wsa) WSACleanup();
}

std::string PhoneLink::computerName()
{
	char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
	DWORD len = sizeof(name);
	if (GetComputerNameA(name, &len)) return name;
	return "This PC";
}

std::vector<std::string> PhoneLink::localAddresses()
{
	std::vector<std::string> out;
	ULONG size = 16 * 1024;
	std::vector<char> buf(size);
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
	if (rc == ERROR_BUFFER_OVERFLOW)
	{
		buf.resize(size);
		rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
	}
	if (rc != NO_ERROR) return out;
	for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next)
	{
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		// Physical LAN / Wi-Fi first; virtual adapters (WSL, VPN) are still
		// listed but after, since phones rarely reach them.
		const bool physical = a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211;
		for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
		{
			const sockaddr_in* sin = (const sockaddr_in*)u->Address.lpSockaddr;
			const std::string ip = AddrToString(*sin);
			if (ip.rfind("169.254.", 0) == 0) continue;
			if (physical && a->FirstGatewayAddress) out.insert(out.begin(), ip);
			else out.push_back(ip);
		}
	}
	return out;
}

bool PhoneLink::start()
{
	if (m_running) return true;
	if (!m_wsa)
	{
		g_app.phase = Phase::Error;
		g_app.setMessage("Networking is not available", "Windows networking (Winsock) failed to start.");
		return false;
	}
	if (!g_frames.open())
	{
		g_app.log(LogLevel::Error, "Could not create the shared video buffer.");
	}
	loadTrusted();

	m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kControlPort);
	addr.sin_addr.s_addr = htonl(kBindAddr);
	BOOL excl = TRUE;
	setsockopt(m_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
	if (m_listen == INVALID_SOCKET || bind(m_listen, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(m_listen, 4) == SOCKET_ERROR)
	{
		const int err = WSAGetLastError();
		if (m_listen != INVALID_SOCKET) closesocket(m_listen);
		m_listen = INVALID_SOCKET;
		g_app.phase = Phase::Error;
		if (err == WSAEADDRINUSE || err == WSAEACCES)
			g_app.setMessage("OpenCamera is already running", "Another copy of OpenCamera (or another program) is using port 65500. Close it and start OpenCamera again.");
		else
			g_app.setMessage("Can't listen for phones", "Windows refused to open network port 65500 (error " + std::to_string(err) + ").");
		return false;
	}

	m_running = true;
	g_app.phase = Phase::Waiting;
	g_app.setMessage("Waiting for your phone", "");
	m_control = std::thread(&PhoneLink::controlLoop, this);
	m_discovery = std::thread(&PhoneLink::discoveryLoop, this);
	g_app.log(LogLevel::Info, "Ready. Listening on port 65500.");
	return true;
}

void PhoneLink::stop()
{
	if (!m_running) return;
	m_running = false;
	m_stopVideo = true;
	if (m_listen != INVALID_SOCKET) closesocket(m_listen);
	if (m_udp != INVALID_SOCKET) closesocket(m_udp);
	m_listen = m_udp = INVALID_SOCKET;
	if (m_control.joinable()) m_control.join();
	if (m_discovery.joinable()) m_discovery.join();
	if (m_video.joinable()) m_video.join();
	g_frames.close();
}

void PhoneLink::loadTrusted()
{
	std::ifstream f(AppState::dataDir() + "\\trusted.txt");
	std::string line;
	while (std::getline(f, line))
		if (!line.empty()) m_trusted.insert(line);
}

void PhoneLink::saveTrusted()
{
	std::ofstream f(AppState::dataDir() + "\\trusted.txt", std::ios::trunc);
	for (const auto& t : m_trusted) f << t << "\n";
}

void PhoneLink::discoveryLoop()
{
	m_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_udp == INVALID_SOCKET) return;
	BOOL yes = TRUE;
	setsockopt(m_udp, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kDiscoveryPort);
	addr.sin_addr.s_addr = htonl(kBindAddr);
	if (bind(m_udp, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		g_app.log(LogLevel::Warning, "Auto-discovery is off (port 65501 busy). Phones can still connect by address.");
		closesocket(m_udp);
		m_udp = INVALID_SOCKET;
		return;
	}
	const std::string reply = "OPENCAMERA1|" + computerName() + "|" + std::to_string(kControlPort);
	sockaddr_in bcast{};
	bcast.sin_family = AF_INET;
	bcast.sin_port = htons(kDiscoveryPort);
	bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	auto lastBeacon = std::chrono::steady_clock::now() - std::chrono::seconds(5);

	while (m_running && m_udp != INVALID_SOCKET)
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - lastBeacon > std::chrono::seconds(1) && g_app.phase.load() != Phase::Live)
		{
			sendto(m_udp, reply.c_str(), (int)reply.size(), 0, (sockaddr*)&bcast, sizeof(bcast));
			lastBeacon = now;
		}
		if (WaitReadable(m_udp, 250) <= 0) continue;
		char buf[256];
		sockaddr_in from{};
		int fromLen = sizeof(from);
		const int n = recvfrom(m_udp, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
		if (n <= 0) continue;
		buf[n] = 0;
		if (strncmp(buf, "OPENCAMERA?", 11) == 0)
			sendto(m_udp, reply.c_str(), (int)reply.size(), 0, (sockaddr*)&from, fromLen);
	}
}

void PhoneLink::controlLoop()
{
	while (m_running)
	{
		g_app.phase = Phase::Waiting;
		g_app.setMessage("Waiting for your phone", "");
		g_app.setPhone("", "");
		g_app.fps = 0;
		g_app.kbps = 0;
		g_frames.setState(ocam::STATE_IDLE);

		SOCKET client = INVALID_SOCKET;
		sockaddr_in peer{};
		while (m_running)
		{
			g_frames.heartbeat();
			if (WaitReadable(m_listen, 250) <= 0) continue;
			int len = sizeof(peer);
			client = accept(m_listen, (sockaddr*)&peer, &len);
			if (client != INVALID_SOCKET) break;
		}
		if (!m_running)
		{
			if (client != INVALID_SOCKET) closesocket(client);
			break;
		}
		BOOL nodelay = TRUE;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

		const std::string host = AddrToString(peer);
		int rtspPort = 0;
		std::string name;
		if (!handshake(client, host, rtspPort, name))
		{
			g_app.log(LogLevel::Warning, "A device at " + host + " connected but did not finish the hello. Ignored.");
			closesocket(client);
			continue;
		}
		const std::string label = name.empty() ? host : name + " (" + host + ")";
		const std::string key = name.empty() ? host : name;
		if (m_blockedThisRun.count(key))
		{
			closesocket(client);
			continue;
		}
		if (!m_trusted.count(key) && !waitApproval(client, key, label))
		{
			closesocket(client);
			continue;
		}
		g_app.setPhone(name.empty() ? "Phone" : name, host);
		g_app.log(LogLevel::Info, "Connected to " + label + ".");
		serveSession(client, host, rtspPort);
		closesocket(client);
		g_app.log(LogLevel::Info, "Phone disconnected.");
	}
	g_frames.setState(ocam::STATE_IDLE);
}

bool PhoneLink::handshake(SOCKET s, const std::string& peer, int& rtspPort, std::string& name)
{
	int32_t ip[4];
	for (int i = 0; i < 4; i++)
		if (!RecvInt(s, ip[i], 5000, m_running)) return false;
	int32_t port = 0;
	if (!RecvInt(s, port, 5000, m_running) || port <= 0 || port > 65535) return false;
	rtspPort = port;
	// The phone also sends its own idea of its IP; the socket's peer address
	// is more reliable (hotspots, several adapters), so we use that.
	(void)peer;

	// Optional name block from the new phone app. Old app sends heartbeats
	// (0x00 0x01), which never match the magic.
	if (WaitReadable(s, 300) > 0)
	{
		uint32_t peek = 0;
		if (recv(s, (char*)&peek, 4, MSG_PEEK) == 4 && ntohl(peek) == kNameMagic)
		{
			int32_t magic = 0, len = 0;
			RecvInt(s, magic, 1000, m_running);
			if (RecvInt(s, len, 1000, m_running) && len > 0 && len <= 128)
			{
				std::string buf((size_t)len, '\0');
				if (RecvExact(s, &buf[0], len, 1000, m_running))
				{
					for (char& c : buf)
						if ((unsigned char)c < 0x20) c = ' ';
					name = buf;
				}
			}
		}
	}
	return true;
}

bool PhoneLink::waitApproval(SOCKET s, const std::string& key, const std::string& label)
{
	g_app.approvalAnswer = 0;
	g_app.setPhone(label, "");
	g_app.phase = Phase::AskApproval;
	g_app.setMessage("Allow this phone?", label + " wants to be your webcam.");
	g_app.log(LogLevel::Info, label + " is asking to connect.");
	const auto end = std::chrono::steady_clock::now() + std::chrono::minutes(2);
	while (m_running && std::chrono::steady_clock::now() < end)
	{
		const int answer = g_app.approvalAnswer.load();
		if (answer == 1)
		{
			if (g_app.approvalRemember)
			{
				m_trusted.insert(key);
				saveTrusted();
			}
			return true;
		}
		if (answer == 2)
		{
			m_blockedThisRun.insert(key);
			g_app.log(LogLevel::Warning, label + " was blocked until OpenCamera restarts.");
			return false;
		}
		// Phone gave up while we were asking?
		if (WaitReadable(s, 100) > 0)
		{
			char tmp[64];
			if (recv(s, tmp, sizeof(tmp), 0) <= 0)
			{
				g_app.log(LogLevel::Info, "The phone stopped asking.");
				return false;
			}
		}
	}
	return false;
}

void PhoneLink::serveSession(SOCKET s, const std::string& host, int rtspPort)
{
	m_stopVideo = false;
	m_videoDone = false;
	g_app.phase = Phase::Connecting;
	g_app.setMessage("Starting video...", "");
	m_video = std::thread(&PhoneLink::videoLoop, this, host, rtspPort);

	auto lastHeard = std::chrono::steady_clock::now();
	std::string why;
	while (m_running)
	{
		g_frames.heartbeat();
		if (g_app.requestDisconnect.exchange(false))
		{
			why = "You disconnected the phone.";
			break;
		}
		const int w = WaitReadable(s, 200);
		if (w < 0)
		{
			why = "The connection to the phone broke.";
			break;
		}
		if (w > 0)
		{
			char tmp[256];
			const int r = recv(s, tmp, sizeof(tmp), 0);
			if (r <= 0)
			{
				why = "The phone closed the connection.";
				break;
			}
			lastHeard = std::chrono::steady_clock::now();
			// New phone app: 'F' = user tapped "Switch camera" on the phone.
			// Heartbeats are 0x01 (new app) or 0x00 0x01 (old app).
			if (memchr(tmp, 'F', (size_t)r))
			{
				g_app.frontCamera = !g_app.frontCamera;
				g_app.requestRestartVideo = true;
				g_app.saveSettings();
				g_app.log(LogLevel::Info, "Switching camera (asked by the phone).");
			}
		}
		else if (std::chrono::steady_clock::now() - lastHeard > std::chrono::seconds(6))
		{
			why = "The phone stopped responding (Wi-Fi lost or app closed?).";
			break;
		}
	}
	m_stopVideo = true;
	if (m_video.joinable()) m_video.join();
	g_frames.setState(ocam::STATE_IDLE);
	if (!why.empty()) g_app.log(LogLevel::Info, why);
	shutdown(s, SD_BOTH);
}

void PhoneLink::videoLoop(std::string host, int port)
{
	VideoReceiver rx;
	int failures = 0;
	while (!m_stopVideo && m_running)
	{
		const QualityPreset& q = kQualityPresets[g_app.quality.load()];
		std::ostringstream url;
		url << "rtsp://" << host << ":" << port << "?h264=" << q.kbps << "-" << q.fps << "-" << q.width << "-" << q.height
		    << "&camera=" << (g_app.frontCamera ? "front" : "back");
		g_app.requestRestartVideo = false;

		g_frames.setState(ocam::STATE_CONNECTING);
		g_app.phase = failures ? Phase::Reconnecting : Phase::Connecting;
		g_app.setMessage(failures ? "Reconnecting video..." : "Starting video...", "");

		VideoReceiver::Options opt;
		opt.gpuDecode = g_app.gpuDecode;
		std::string err;
		if (!rx.open(url.str(), opt, &m_stopVideo, err))
		{
			if (m_stopVideo) break;
			failures++;
			g_app.log(LogLevel::Warning, err);
			if (failures >= 3)
			{
				g_app.phase = Phase::Reconnecting;
				g_app.setMessage("Can't get video from the phone", err + " Retrying...");
			}
			for (int i = 0; i < 10 && !m_stopVideo; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		g_app.hwDecodeActive = rx.hwActive();
		g_app.log(LogLevel::Info, std::string("Video started (") + q.label + ", " + (rx.hwActive() ? "GPU" : "CPU") + " decoding).");
		g_frames.setMirror(g_app.mirror);

		int frames = 0;
		int64_t lastBytes = 0;
		double procSum = 0;
		auto window = std::chrono::steady_clock::now();
		bool live = false;
		while (!m_stopVideo && !g_app.requestRestartVideo)
		{
			const int r = rx.step(g_frames);
			if (r < 0)
			{
				if (!m_stopVideo) g_app.log(LogLevel::Warning, rx.lastError());
				break;
			}
			if (r == 1)
			{
				if (!live)
				{
					live = true;
					failures = 0;
					g_frames.setState(ocam::STATE_LIVE);
					g_app.phase = Phase::Live;
					g_app.setMessage("Live", "");
				}
				frames++;
				procSum += rx.lastProcessMs();
			}
			g_frames.setMirror(g_app.mirror);
			const auto now = std::chrono::steady_clock::now();
			const double secs = std::chrono::duration<double>(now - window).count();
			if (secs >= 1.0)
			{
				g_app.fps = (float)(frames / secs);
				g_app.kbps = (float)((rx.bytesReceived() - lastBytes) * 8.0 / 1000.0 / secs);
				g_app.decodeMs = frames ? (float)(procSum / frames) : 0.f;
				g_app.videoWidth = rx.width();
				g_app.videoHeight = rx.height();
				g_app.hwDecodeActive = rx.hwActive();
				lastBytes = rx.bytesReceived();
				frames = 0;
				procSum = 0;
				window = now;
			}
		}
		rx.close();
		g_app.fps = 0;
		g_app.kbps = 0;
		if (!m_stopVideo && !g_app.requestRestartVideo)
		{
			failures++;
			for (int i = 0; i < 5 && !m_stopVideo; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
	g_frames.setState(ocam::STATE_IDLE);
	m_videoDone = true;
}
