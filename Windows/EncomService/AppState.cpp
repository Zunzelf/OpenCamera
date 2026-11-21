#include "AppState.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <ctime>
#include <fstream>
#include <sstream>

AppState g_app;

void AppState::setMessage(const std::string& headline, const std::string& detail)
{
	std::lock_guard<std::mutex> g(m_lock);
	m_headline = headline;
	m_detail = detail;
}

void AppState::getMessage(std::string& headline, std::string& detail)
{
	std::lock_guard<std::mutex> g(m_lock);
	headline = m_headline;
	detail = m_detail;
}

void AppState::setPhone(const std::string& name, const std::string& address)
{
	std::lock_guard<std::mutex> g(m_lock);
	m_phoneName = name;
	m_phoneAddress = address;
}

void AppState::getPhone(std::string& name, std::string& address)
{
	std::lock_guard<std::mutex> g(m_lock);
	name = m_phoneName;
	address = m_phoneAddress;
}

void AppState::log(LogLevel level, const std::string& text)
{
	char buf[16];
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
	localtime_s(&tmv, &t);
	std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
	std::lock_guard<std::mutex> g(m_lock);
	m_log.push_back({ level, text, buf });
	while (m_log.size() > 300) m_log.pop_front();
}

std::vector<LogLine> AppState::logSnapshot()
{
	std::lock_guard<std::mutex> g(m_lock);
	return std::vector<LogLine>(m_log.begin(), m_log.end());
}

void AppState::clearLog()
{
	std::lock_guard<std::mutex> g(m_lock);
	m_log.clear();
}

std::string AppState::dataDir()
{
	char path[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path)))
	{
		std::string dir = std::string(path) + "\\OpenCamera";
		CreateDirectoryA(dir.c_str(), NULL);
		return dir;
	}
	return ".";
}

void AppState::loadSettings()
{
	std::ifstream f(dataDir() + "\\settings.ini");
	std::string line;
	while (std::getline(f, line))
	{
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string k = line.substr(0, eq);
		const int v = atoi(line.c_str() + eq + 1);
		if (k == "quality" && v >= 0 && v < kQualityPresetCount) quality = v;
		else if (k == "front") frontCamera = v != 0;
		else if (k == "mirror") mirror = v != 0;
		else if (k == "gpu") gpuDecode = v != 0;
	}
}

void AppState::saveSettings()
{
	std::ofstream f(dataDir() + "\\settings.ini", std::ios::trunc);
	f << "quality=" << quality.load() << "\n"
	  << "front=" << (frontCamera ? 1 : 0) << "\n"
	  << "mirror=" << (mirror ? 1 : 0) << "\n"
	  << "gpu=" << (gpuDecode ? 1 : 0) << "\n";
}
