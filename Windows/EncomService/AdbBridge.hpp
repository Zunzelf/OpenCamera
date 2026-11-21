#pragma once
// USB cable without tethering: uses adb (USB debugging) when it is on.
//   adb reverse tcp:65500 / tcp:65501  -> the phone reaches this PC at 127.0.0.1
//   adb forward tcp:P tcp:P            -> this PC reaches the phone's RTSP port
// Video then runs as RTP over the RTSP TCP connection (adb carries TCP only).
// adb.exe is looked for next to OpenCamera, on PATH and in the Android SDK.

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>

class AdbBridge
{
public:
	~AdbBridge() { stop(); }
	void start();
	void stop();

	// Serial of the phone that is ready over the cable, or empty.
	std::string serial();
	// Map 127.0.0.1:port on this PC to the phone's port. False if adb failed.
	bool forward(int port);

private:
	void loop();
	bool run(const std::wstring& args, std::string& out, int timeoutMs = 4000);
	std::wstring findAdb();

	std::thread m_thread;
	std::atomic<bool> m_running{ false };
	std::mutex m_lock;
	std::wstring m_adb;
	std::string m_serial;
	std::set<std::string> m_reversed;   // serials with reverse set up
	std::set<std::string> m_warned;     // serials we told the user about (unauthorized)
	unsigned m_checkTick = 0;
	int m_clientVersion = 0;            // from "adb version", e.g. 41
	bool m_warnedMismatch = false;
};
