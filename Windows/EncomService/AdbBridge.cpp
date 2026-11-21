#include "AdbBridge.hpp"
#include "AppState.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

static std::wstring Widen(const std::string& s)
{
	return std::wstring(s.begin(), s.end()); // serials and numbers are ASCII
}

static bool FileExists(const std::wstring& p)
{
	const DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring EnvDir(const wchar_t* name)
{
	wchar_t buf[MAX_PATH] = {};
	const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
	return n > 0 && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}

std::wstring AdbBridge::findAdb()
{
	std::vector<std::wstring> candidates;
	wchar_t self[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, self, MAX_PATH))
	{
		std::wstring dir(self);
		dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
		candidates.push_back(dir + L"adb.exe");
		candidates.push_back(dir + L"platform-tools\\adb.exe");
	}
	wchar_t found[MAX_PATH] = {};
	if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) candidates.push_back(found);
	for (const wchar_t* v : { L"ANDROID_HOME", L"ANDROID_SDK_ROOT" })
	{
		const std::wstring d = EnvDir(v);
		if (!d.empty()) candidates.push_back(d + L"\\platform-tools\\adb.exe");
	}
	const std::wstring local = EnvDir(L"LOCALAPPDATA");
	if (!local.empty()) candidates.push_back(local + L"\\Android\\Sdk\\platform-tools\\adb.exe");
	for (const auto& c : candidates)
		if (FileExists(c)) return c;
	return L"";
}

bool AdbBridge::run(const std::wstring& args, std::string& out, int timeoutMs)
{
	out.clear();
	SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
	HANDLE rd = nullptr, wr = nullptr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	// Hand adb ONLY the output pipe. Plain inheritance would also pass our
	// sockets (Winsock handles are inheritable), and the adb server that adb
	// may start would keep port 65500 open after OpenCamera exits.
	SIZE_T attrSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
	std::vector<unsigned char> attrBuf(attrSize);
	auto* attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
	HANDLE inherit[1] = { wr };
	if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
		!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr))
	{
		CloseHandle(rd);
		CloseHandle(wr);
		return false;
	}
	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdOutput = wr;
	si.StartupInfo.hStdError = wr;
	si.StartupInfo.hStdInput = nullptr;
	si.lpAttributeList = attrs;
	PROCESS_INFORMATION pi{};
	std::wstring cmd = L"\"" + m_adb + L"\" " + args;
	std::vector<wchar_t> line(cmd.begin(), cmd.end());
	line.push_back(0);
	const BOOL ok = CreateProcessW(m_adb.c_str(), line.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
	DeleteProcThreadAttributeList(attrs);
	CloseHandle(wr);
	if (!ok)
	{
		CloseHandle(rd);
		return false;
	}
	// Read until the process closes the pipe (or the timeout passes).
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	char buf[1024];
	for (;;)
	{
		DWORD avail = 0;
		if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break; // closed
		if (avail)
		{
			DWORD got = 0;
			if (!ReadFile(rd, buf, (DWORD)(std::min<size_t>)(sizeof(buf), avail), &got, nullptr) || !got) break;
			out.append(buf, got);
			continue;
		}
		if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0)
		{
			// drain what is left
			while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail)
			{
				DWORD got = 0;
				if (!ReadFile(rd, buf, (DWORD)(std::min<size_t>)(sizeof(buf), avail), &got, nullptr) || !got) break;
				out.append(buf, got);
			}
			break;
		}
		if (std::chrono::steady_clock::now() > end || !m_running)
		{
			TerminateProcess(pi.hProcess, 1);
			break;
		}
	}
	DWORD code = 1;
	WaitForSingleObject(pi.hProcess, 1000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(rd);
	return code == 0;
}

void AdbBridge::start()
{
	if (m_running) return;
	m_running = true;
	m_thread = std::thread(&AdbBridge::loop, this);
}

void AdbBridge::stop()
{
	m_running = false;
	if (m_thread.joinable()) m_thread.join();
}

std::string AdbBridge::serial()
{
	std::lock_guard<std::mutex> g(m_lock);
	return m_serial;
}

bool AdbBridge::forward(int port)
{
	const std::string s = serial();
	if (s.empty() || m_adb.empty()) return false;
	std::string out;
	const std::wstring p = std::to_wstring(port);
	return run(L"-s " + Widen(s) + L" forward tcp:" + p + L" tcp:" + p, out);
}

void AdbBridge::loop()
{
	auto nextSearch = std::chrono::steady_clock::now();
	bool announcedAdb = false;
	while (m_running)
	{
		const auto now = std::chrono::steady_clock::now();
		if (m_adb.empty() && now >= nextSearch)
		{
			m_adb = findAdb();
			nextSearch = now + std::chrono::seconds(30);
			if (!m_adb.empty() && !announcedAdb)
			{
				announcedAdb = true;
				g_app.log(LogLevel::Info, "USB cable mode is available (adb found). Plug in a phone with USB debugging on.");
			}
		}
		if (!m_adb.empty())
		{
			std::string out;
			std::set<std::string> ready, seen;
			if (run(L"devices", out))
			{
				std::istringstream in(out);
				std::string line;
				while (std::getline(in, line))
				{
					if (!line.empty() && line.back() == '\r') line.pop_back();
					const size_t tab = line.find('\t');
					if (tab == std::string::npos) continue;
					const std::string serial = line.substr(0, tab), state = line.substr(tab + 1);
					if (serial.find(':') != std::string::npos || serial.rfind("emulator-", 0) == 0) continue; // Wi-Fi adb / emulators
					seen.insert(serial);
					if (state == "device") ready.insert(serial);
					else if (state == "unauthorized" && !m_warned.count(serial))
					{
						m_warned.insert(serial);
						g_app.log(LogLevel::Warning, "A phone is plugged in but USB debugging isn't allowed for this PC yet. Tap \"Allow\" on the phone to use the cable.");
					}
				}
			}
			// Another tool may restart adb (scrcpy, Android Studio), which drops our
			// mapping: check now and then, and set it up again.
			if (++m_checkTick % 3 == 0)
				for (auto it = m_reversed.begin(); it != m_reversed.end();)
				{
					std::string list;
					run(L"-s " + Widen(*it) + L" reverse --list", list);
					it = list.find("tcp:65500") == std::string::npos ? m_reversed.erase(it) : std::next(it);
				}
			for (const auto& s : ready)
			{
				if (m_reversed.count(s)) continue;
				std::string o1, o2;
				const std::wstring w = Widen(s);
				if (run(L"-s " + w + L" reverse tcp:65500 tcp:65500", o1) && run(L"-s " + w + L" reverse tcp:65501 tcp:65501", o2))
				{
					m_reversed.insert(s);
					g_app.log(LogLevel::Info, "USB cable ready: the phone can now connect through the cable.");
				}
			}
			// forget unplugged phones so they are set up again next time
			for (auto it = m_reversed.begin(); it != m_reversed.end();)
				it = ready.count(*it) ? std::next(it) : m_reversed.erase(it);
			for (auto it = m_warned.begin(); it != m_warned.end();)
				it = seen.count(*it) ? std::next(it) : m_warned.erase(it);
			{
				std::lock_guard<std::mutex> g(m_lock);
				m_serial = m_reversed.empty() ? std::string() : *m_reversed.begin();
			}
		}
		for (int i = 0; i < 20 && m_running; i++) Sleep(100);
	}
}
