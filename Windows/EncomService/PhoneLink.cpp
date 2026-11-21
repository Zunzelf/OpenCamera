#include "PhoneLink.hpp"
#include "AppState.hpp"
#include "VideoReceiver.hpp"
#include "../shared/FrameShm.h"

#include <WS2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
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

// Which kind of adapter carries this connection, from the PC's own address on it.
// USB tethering shows up in Windows as a "Remote NDIS" (RNDIS) or NCM adapter.
static int LinkKind(SOCKET s)
{
	sockaddr_in local{};
	int len = sizeof(local);
	if (getsockname(s, (sockaddr*)&local, &len) != 0) return 0;
	if (local.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) return 4;
	ULONG size = 16 * 1024;
	std::vector<unsigned char> buf(size);
	ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
		(IP_ADAPTER_ADDRESSES*)buf.data(), &size);
	if (rc == ERROR_BUFFER_OVERFLOW)
	{
		buf.resize(size);
		rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
			(IP_ADAPTER_ADDRESSES*)buf.data(), &size);
	}
	if (rc != NO_ERROR) return 0;
	for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next)
	{
		for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
		{
			const auto* sa = (const sockaddr_in*)u->Address.lpSockaddr;
			if (sa->sin_family != AF_INET || sa->sin_addr.s_addr != local.sin_addr.s_addr) continue;
			if (a->IfType == IF_TYPE_IEEE80211) return 1;
			std::wstring d = a->Description ? a->Description : L"";
			for (auto& c : d) c = (wchar_t)towlower(c);
			if (d.find(L"ndis") != std::wstring::npos || d.find(L"usb") != std::wstring::npos ||
				d.find(L"android") != std::wstring::npos || d.find(L"ncm") != std::wstring::npos)
				return 2;
			// Not the cable: the PC may be wired to the router, but the phone is on Wi-Fi.
			return a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ? 4 : 1;
		}
	}
	return 0;
}

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

static bool SendAll(SOCKET s, const std::string& data)
{
	const char* p = data.data();
	int left = (int)data.size();
	while (left > 0)
	{
		const int n = send(s, p, left, 0);
		if (n <= 0) return false;
		p += n;
		left -= n;
	}
	return true;
}

// "key=value key=value" -> map
static std::map<std::string, std::string> ParseKv(const std::string& text)
{
	std::map<std::string, std::string> m;
	std::istringstream in(text);
	std::string tok;
	while (in >> tok)
	{
		const size_t eq = tok.find('=');
		if (eq != std::string::npos && eq > 0) m[tok.substr(0, eq)] = tok.substr(eq + 1);
	}
	return m;
}

static int KvInt(const std::map<std::string, std::string>& m, const char* k, int def)
{
	auto it = m.find(k);
	return it == m.end() ? def : atoi(it->second.c_str());
}

static int64_t KvInt64(const std::map<std::string, std::string>& m, const char* k, int64_t def)
{
	auto it = m.find(k);
	return it == m.end() ? def : _atoi64(it->second.c_str());
}

static float KvFloat(const std::map<std::string, std::string>& m, const char* k, float def)
{
	auto it = m.find(k);
	return it == m.end() ? def : (float)atof(it->second.c_str());
}

// "a<sep>b" -> two numbers
template <typename T>
static bool KvPair(const std::map<std::string, std::string>& m, const char* k, char sep, T& a, T& b)
{
	auto it = m.find(k);
	if (it == m.end()) return false;
	const size_t pos = it->second.find(sep, 1); // skip a leading minus sign
	if (pos == std::string::npos) return false;
	a = (T)atof(it->second.substr(0, pos).c_str());
	b = (T)atof(it->second.substr(pos + 1).c_str());
	return true;
}

static PhoneCaps ParseCaps(const std::string& text)
{
	const auto m = ParseKv(text);
	PhoneCaps c;
	c.valid = true;
	c.front = KvInt(m, "front", 0) != 0;
	c.manual = KvInt(m, "manual", 0) != 0;
	c.kelvin = KvInt(m, "wbk", 0) != 0;
	c.torch = KvInt(m, "torch", 0) != 0;
	c.ois = KvInt(m, "ois", 0) != 0;
	c.eis = KvInt(m, "eis", 0) != 0;
	c.point = KvInt(m, "point", 0) != 0;
	KvPair(m, "iso", '-', c.isoMin, c.isoMax);
	double e0 = 0, e1 = 0;
	if (KvPair(m, "exp", '-', e0, e1)) { c.expMin = (int64_t)e0; c.expMax = (int64_t)e1; }
	KvPair(m, "ev", ',', c.evMin, c.evMax);
	c.evStep = KvFloat(m, "evstep", 1.0f / 3.0f);
	c.minFocus = KvFloat(m, "mf", 0);
	KvPair(m, "zoom", ',', c.zoomMin, c.zoomMax);
	auto it = m.find("fps");
	if (it != m.end())
	{
		std::istringstream sizes(it->second);
		std::string entry;
		while (std::getline(sizes, entry, ';'))
		{
			const size_t colon = entry.find(':');
			if (colon == std::string::npos) continue;
			std::pair<int, std::vector<int>> row;
			row.first = atoi(entry.substr(0, colon).c_str());
			std::istringstream rates(entry.substr(colon + 1));
			std::string r;
			while (std::getline(rates, r, ','))
				if (!r.empty()) row.second.push_back(atoi(r.c_str()));
			c.fps.push_back(row);
		}
	}
	return c;
}

static PhoneStatus ParseStatus(const std::string& text)
{
	const auto m = ParseKv(text);
	PhoneStatus st;
	st.valid = true;
	st.iso = KvInt(m, "iso", 0);
	st.expNs = KvInt64(m, "exp", 0);
	st.focus = KvFloat(m, "fd", -1);
	st.afState = KvInt(m, "af", -1);
	st.aeState = KvInt(m, "ae", -1);
	st.torch = KvInt(m, "torch", 0) != 0;
	st.battery = KvInt(m, "bat", -1);
	st.temperature = KvInt(m, "temp", -1);
	st.charging = KvInt(m, "chg", 0) != 0;
	st.rotation = KvInt(m, "rot", -1);
	st.level = KvInt(m, "level", 0);
	return st;
}

// The settings line sent to the phone. iso/exp may be overridden by the
// PC's own auto-exposure helper (see serveSession).
static std::string ProLine(const ProSettings& p, int iso, int64_t expNs)
{
	static const char* wb[] = { "auto", "daylight", "cloudy", "shade", "tungsten", "fluorescent", "k" };
	static const char* flicker[] = { "auto", "50", "60", "off" };
	char buf[512];
	snprintf(buf, sizeof(buf),
		"SET iso=%d exp=%lld ev=%d wb=%s k=%d af=%s fd=%.3f ael=%d awbl=%d afl=%d zoom=%.2f torch=%d ois=%d eis=%d flicker=%s\n",
		iso, (long long)expNs, p.ev, wb[std::min(std::max(p.wb, 0), 6)], p.kelvin, p.manualFocus ? "m" : "c", p.focus,
		p.aeLock ? 1 : 0, p.awbLock ? 1 : 0, p.afLock ? 1 : 0, p.zoom, p.torch ? 1 : 0, p.ois ? 1 : 0, p.eis ? 1 : 0,
		flicker[std::min(std::max(p.flicker, 0), 3)]);
	return buf;
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
#ifndef OPENCAMERA_LOOPBACK_TEST
	m_adb.start();
#endif
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
	m_adb.stop();
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
	// Phones on the USB cable ask over TCP (adb reverse carries TCP only). Loopback only.
	m_tcpProbe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	{
		sockaddr_in lo{};
		lo.sin_family = AF_INET;
		lo.sin_port = htons(kDiscoveryPort);
		lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (m_tcpProbe != INVALID_SOCKET && (bind(m_tcpProbe, (sockaddr*)&lo, sizeof(lo)) == SOCKET_ERROR || listen(m_tcpProbe, 4) == SOCKET_ERROR))
		{
			closesocket(m_tcpProbe);
			m_tcpProbe = INVALID_SOCKET;
		}
	}
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
		if (m_tcpProbe != INVALID_SOCKET && WaitReadable(m_tcpProbe, 0) > 0)
		{
			SOCKET c = accept(m_tcpProbe, nullptr, nullptr);
			if (c != INVALID_SOCKET)
			{
				char q[32] = {};
				if (WaitReadable(c, 500) > 0 && recv(c, q, sizeof(q) - 1, 0) > 0 && strncmp(q, "OPENCAMERA?", 11) == 0)
				{
					const std::string line = reply + "\n";
					send(c, line.c_str(), (int)line.size(), 0);
				}
				closesocket(c);
			}
		}
		if (WaitReadable(m_udp, 150) <= 0) continue;
		char buf[256];
		sockaddr_in from{};
		int fromLen = sizeof(from);
		const int n = recvfrom(m_udp, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
		if (n <= 0) continue;
		buf[n] = 0;
		if (strncmp(buf, "OPENCAMERA?", 11) == 0)
			sendto(m_udp, reply.c_str(), (int)reply.size(), 0, (sockaddr*)&from, fromLen);
	}
	if (m_tcpProbe != INVALID_SOCKET) closesocket(m_tcpProbe);
	m_tcpProbe = INVALID_SOCKET;
}

void PhoneLink::controlLoop()
{
	while (m_running)
	{
		g_app.phase = Phase::Waiting;
		g_app.setMessage("Waiting for your phone", "");
		g_app.setPhone("", "");
		g_app.linkKind = 0;
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
		// Through adb the phone shows up as 127.0.0.1: that is the USB cable.
		g_app.linkKind = (peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && !m_adb.serial().empty()) ? 2 : LinkKind(client);
		g_app.setPhone(name.empty() ? "Phone" : name, host);
		{
			static const char* kinds[] = { "", " over Wi-Fi", " over the USB cable", " over a network cable", " on this PC" };
			const int k = g_app.linkKind.load();
			g_app.log(LogLevel::Info, "Connected to " + label + (k > 0 && k < 5 ? kinds[k] : "") + ".");
		}
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
	// Through the USB cable (adb) it can arrive in pieces, so wait for 4 bytes.
	uint32_t peek = 0;
	int peeked = 0;
	for (int i = 0; i < 75 && m_running; i++)
	{
		if (WaitReadable(s, 20) > 0)
		{
			peeked = recv(s, (char*)&peek, 4, MSG_PEEK);
			if (peeked >= 4 || peeked <= 0) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(20)); // part of it so far
		}
	}
	if (peeked == 4)
	{
		if (ntohl(peek) == kNameMagic)
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

	g_app.clearPhoneInfo();
	g_app.phoneTalksBack = false;
	// Phones with pro controls answer this; older phone apps never read it.
	SendAll(s, "HELLO 2\n");
	g_app.proDirty = true;
	g_app.pointDirty = false;

	// PC-side auto exposure for "shutter manual + ISO auto" (and the reverse):
	// the phone can only do fully auto or fully manual, so the PC nudges the
	// auto value from the picture's brightness 4 times a second.
	int semiIso = 0;
	int64_t semiExp = 0;
	int sentIso = -1;
	int64_t sentExp = -1;
	auto lastAe = std::chrono::steady_clock::now();
	bool loggedPro = false;

	auto sendPro = [&]() {
		const ProSettings p = g_app.getPro();
		int iso = p.iso;
		int64_t exp = p.expNs;
		if (p.iso == 0 && p.expNs > 0) iso = semiIso;
		if (p.expNs == 0 && p.iso > 0) exp = semiExp;
		sentIso = iso;
		sentExp = exp;
		return SendAll(s, ProLine(p, iso, exp));
	};

	auto handleMessage = [&](const std::string& msg) {
		if (msg.rfind("CAPS ", 0) == 0)
		{
			g_app.setCaps(ParseCaps(msg.substr(5)));
			g_app.phoneTalksBack = true;
			if (!loggedPro)
			{
				loggedPro = true;
				const PhoneCaps c = g_app.getCaps();
				g_app.log(LogLevel::Info, std::string("Pro controls ready (") + (c.manual ? "manual ISO and shutter" : "automatic exposure only") +
					(c.kelvin ? ", Kelvin white balance" : "") + ").");
			}
		}
		else if (msg.rfind("STAT ", 0) == 0)
		{
			g_app.setStatus(ParseStatus(msg.substr(5)));
		}
		else if (msg.rfind("EVT ", 0) == 0)
		{
			const auto m = ParseKv(msg.substr(4));
			if (m.count("torch"))
			{
				ProSettings p = g_app.getPro();
				p.torch = KvInt(m, "torch", 0) != 0;
				g_app.setPro(p, false);
			}
		}
	};

	auto lastHeard = std::chrono::steady_clock::now();
	std::string why;
	std::string inbuf;
	while (m_running)
	{
		g_frames.heartbeat();
		if (g_app.requestDisconnect.exchange(false))
		{
			why = "You disconnected the phone.";
			break;
		}
		if (g_app.proDirty.exchange(false) && !sendPro())
		{
			why = "The connection to the phone broke.";
			break;
		}
		if (g_app.pointDirty.exchange(false))
		{
			float x, y;
			g_app.getPoint(x, y);
			char buf[64];
			if (x < 0) snprintf(buf, sizeof(buf), "POINT none\n");
			else snprintf(buf, sizeof(buf), "POINT %.4f %.4f\n", x, y);
			SendAll(s, buf);
		}

		const auto now = std::chrono::steady_clock::now();
		if (now - lastAe >= std::chrono::milliseconds(250))
		{
			lastAe = now;
			const ProSettings p = g_app.getPro();
			const PhoneCaps c = g_app.getCaps();
			const bool isoAuto = p.iso == 0, expAuto = p.expNs == 0;
			if (c.valid && c.manual && isoAuto != expAuto && g_app.phase.load() == Phase::Live)
			{
				const PhoneStatus st = g_app.getStatus();
				const int fps = std::max(1, g_app.frameRate.load());
				if (semiIso <= 0) semiIso = st.iso > 0 ? st.iso : 400;
				if (semiExp <= 0) semiExp = st.expNs > 0 ? st.expNs : 1000000000LL / (2 * fps);
				const float mean = g_app.getLuma(nullptr);
				if (mean > 1.0f)
				{
					const float target = std::min(225.0f, std::max(25.0f, 118.0f * powf(2.0f, p.ev * c.evStep * 0.45f)));
					const float ratio = target / mean;
					if (fabsf(ratio - 1.0f) > 0.05f)
					{
						// Picture values are gamma-encoded (~2.2): exposure ~ ratio^2.2,
						// taken half-way per step so it settles without pumping.
						const float change = std::min(1.45f, std::max(0.7f, powf(ratio, 1.1f)));
						if (isoAuto)
							semiIso = std::min(c.isoMax, std::max(c.isoMin, (int)(semiIso * change)));
						else
							semiExp = std::min(std::min(c.expMax, 1000000000LL / fps), std::max(c.expMin, (int64_t)(semiExp * change)));
						const bool moved = isoAuto ? std::abs(semiIso - sentIso) > sentIso * 0.03
						                           : std::llabs(semiExp - sentExp) > sentExp * 0.03;
						if (moved) sendPro();
					}
				}
			}
			else
			{
				semiIso = 0;
				semiExp = 0;
			}
		}

		const int w = WaitReadable(s, 100);
		if (w < 0)
		{
			why = "The connection to the phone broke.";
			break;
		}
		if (w > 0)
		{
			char tmp[4096];
			const int r = recv(s, tmp, sizeof(tmp), 0);
			if (r <= 0)
			{
				why = "The phone closed the connection.";
				break;
			}
			lastHeard = std::chrono::steady_clock::now();
			inbuf.append(tmp, (size_t)r);
			// Stream from the phone: heartbeats (0x01; old app 0x00 0x01),
			// 'F' = "Switch camera" tapped on the phone, and framed messages
			// 'M' + int32 length + text (phones with pro controls).
			size_t i = 0;
			while (i < inbuf.size())
			{
				const unsigned char c = (unsigned char)inbuf[i];
				if (c == 'M')
				{
					if (inbuf.size() - i < 5) break;
					uint32_t be = 0;
					memcpy(&be, inbuf.data() + i + 1, 4);
					const uint32_t len = ntohl(be);
					if (len > 65536) { i++; continue; }
					if (inbuf.size() - i - 5 < len) break;
					handleMessage(inbuf.substr(i + 5, len));
					i += 5 + len;
				}
				else
				{
					if (c == 'F')
					{
						g_app.frontCamera = !g_app.frontCamera;
						g_app.requestRestartVideo = true;
						g_app.saveSettings();
						g_app.log(LogLevel::Info, "Switching camera (asked by the phone).");
					}
					i++;
				}
			}
			inbuf.erase(0, i);
		}
		else if (std::chrono::steady_clock::now() - lastHeard > std::chrono::seconds(6))
		{
			why = "The phone stopped responding (Wi-Fi lost or app closed?).";
			break;
		}
	}
	g_app.clearPhoneInfo();
	g_app.phoneTalksBack = false;
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
		const int fps = std::max(10, std::min(120, g_app.frameRate.load()));
		const int kbps = (int)(q.kbps * std::max(1.0, fps / 30.0)); // more frames need more bits
		// USB cable: reach the phone's RTSP port through adb, RTP inside the RTSP connection.
		const bool viaAdb = host == "127.0.0.1" && !m_adb.serial().empty();
		if (viaAdb && !m_adb.forward(port))
		{
			g_app.log(LogLevel::Warning, "Couldn't reach the phone through the USB cable (adb forward failed). Retrying...");
			for (int i = 0; i < 20 && !m_stopVideo; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		url << "rtsp://" << host << ":" << port << "?h264=" << kbps << "-" << fps << "-" << q.width << "-" << q.height
		    << "&camera=" << (g_app.frontCamera ? "front" : "back");
		g_app.requestRestartVideo = false;

		g_frames.setState(ocam::STATE_CONNECTING);
		g_app.phase = failures ? Phase::Reconnecting : Phase::Connecting;
		g_app.setMessage(failures ? "Reconnecting video..." : "Starting video...", "");

		VideoReceiver::Options opt;
		opt.gpuDecode = g_app.gpuDecode;
		opt.tcp = viaAdb;
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
		g_app.log(LogLevel::Info, std::string("Video started (") + q.label + " " + std::to_string(fps) + " fps, " + (rx.hwActive() ? "GPU" : "CPU") + " decoding).");
		g_frames.setMirror(g_app.mirror);

		int frames = 0;
		int64_t lastBytes = 0;
		double procSum = 0;
		auto window = std::chrono::steady_clock::now();
		bool live = false;
		while (!m_stopVideo && !g_app.requestRestartVideo)
		{
			// Rotation: fixed by the user, or what the phone says is upright.
			const int mode = g_app.rotationMode.load();
			int rot = (mode - 1) * 90;
			if (mode == 0)
			{
				const PhoneStatus st = g_app.getStatus();
				rot = st.valid && st.rotation >= 0 ? st.rotation : 0;
			}
			rx.setTransform(rot, g_app.fillFrame);
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
				g_app.appliedRotation = rx.appliedRotation();
				g_app.cropTop = rx.cropTop();
				g_app.cropHeight = rx.cropHeight();
				g_app.setLuma(rx.histogram(), rx.meanLuma());
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
