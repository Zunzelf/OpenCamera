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

ProSettings AppState::getPro()
{
	std::lock_guard<std::mutex> g(m_lock);
	return m_pro;
}

void AppState::setPro(const ProSettings& p, bool sendToPhone)
{
	{
		std::lock_guard<std::mutex> g(m_lock);
		m_pro = p;
	}
	if (sendToPhone) proDirty = true;
}

PhoneCaps AppState::getCaps()
{
	std::lock_guard<std::mutex> g(m_lock);
	return m_caps;
}

void AppState::setCaps(const PhoneCaps& c)
{
	std::lock_guard<std::mutex> g(m_lock);
	m_caps = c;
}

PhoneStatus AppState::getStatus()
{
	std::lock_guard<std::mutex> g(m_lock);
	return m_status;
}

void AppState::setStatus(const PhoneStatus& s)
{
	std::lock_guard<std::mutex> g(m_lock);
	m_status = s;
}

void AppState::clearPhoneInfo()
{
	std::lock_guard<std::mutex> g(m_lock);
	m_caps = PhoneCaps();
	m_status = PhoneStatus();
	m_pointX = m_pointY = -1;
	m_hist.fill(0);
	m_meanLuma = 0;
}

void AppState::setPoint(float x, float y)
{
	{
		std::lock_guard<std::mutex> g(m_lock);
		m_pointX = x;
		m_pointY = y;
	}
	pointDirty = true;
}

void AppState::getPoint(float& x, float& y)
{
	std::lock_guard<std::mutex> g(m_lock);
	x = m_pointX;
	y = m_pointY;
}

void AppState::setLuma(const float* hist64, float mean)
{
	std::lock_guard<std::mutex> g(m_lock);
	for (int i = 0; i < 64; i++) m_hist[i] = hist64[i];
	m_meanLuma = mean;
}

float AppState::getLuma(float* hist64)
{
	std::lock_guard<std::mutex> g(m_lock);
	if (hist64)
		for (int i = 0; i < 64; i++) hist64[i] = m_hist[i];
	return m_meanLuma;
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
		const char* val = line.c_str() + eq + 1;
		const int v = atoi(val);
		if (k == "quality" && v >= 0 && v < kQualityPresetCount) quality = v;
		else if (k == "front") frontCamera = v != 0;
		else if (k == "mirror") mirror = v != 0;
		else if (k == "gpu") gpuDecode = v != 0;
		else if (k == "fps" && v >= 10 && v <= 120) frameRate = v;
		else if (k == "rotation" && v >= 0 && v <= 4) rotationMode = v;
		else if (k == "fill") fillFrame = v != 0;
		else if (k == "grid") showGrid = v != 0;
		else if (k == "histogram") showHistogram = v != 0;
		else if (k == "zebra") showZebra = v != 0;
		else if (k == "peaking") showPeaking = v != 0;
		else if (k == "level") showLevel = v != 0;
		else if (k == "pro_iso") m_pro.iso = v;
		else if (k == "pro_exp") m_pro.expNs = _atoi64(val);
		else if (k == "pro_ev") m_pro.ev = v;
		else if (k == "pro_wb") m_pro.wb = v;
		else if (k == "pro_kelvin") m_pro.kelvin = v;
		else if (k == "pro_mf") m_pro.manualFocus = v != 0;
		else if (k == "pro_focus") m_pro.focus = (float)atof(val);
		else if (k == "pro_zoom") m_pro.zoom = (float)atof(val);
		else if (k == "pro_ois") m_pro.ois = v != 0;
		else if (k == "pro_eis") m_pro.eis = v != 0;
		else if (k == "pro_flicker") m_pro.flicker = v;
	}
}

void AppState::saveSettings()
{
	std::ofstream f(dataDir() + "\\settings.ini", std::ios::trunc);
	f << "quality=" << quality.load() << "\n"
	  << "front=" << (frontCamera ? 1 : 0) << "\n"
	  << "mirror=" << (mirror ? 1 : 0) << "\n"
	  << "gpu=" << (gpuDecode ? 1 : 0) << "\n"
	  << "fps=" << frameRate.load() << "\n"
	  << "rotation=" << rotationMode.load() << "\n"
	  << "fill=" << (fillFrame ? 1 : 0) << "\n"
	  << "grid=" << (showGrid ? 1 : 0) << "\n"
	  << "histogram=" << (showHistogram ? 1 : 0) << "\n"
	  << "zebra=" << (showZebra ? 1 : 0) << "\n"
	  << "peaking=" << (showPeaking ? 1 : 0) << "\n"
	  << "level=" << (showLevel ? 1 : 0) << "\n";
	// Locks, torch and the focus point are per session: not remembered.
	const ProSettings p = getPro();
	f << "pro_iso=" << p.iso << "\n"
	  << "pro_exp=" << p.expNs << "\n"
	  << "pro_ev=" << p.ev << "\n"
	  << "pro_wb=" << p.wb << "\n"
	  << "pro_kelvin=" << p.kelvin << "\n"
	  << "pro_mf=" << (p.manualFocus ? 1 : 0) << "\n"
	  << "pro_focus=" << p.focus << "\n"
	  << "pro_zoom=" << p.zoom << "\n"
	  << "pro_ois=" << (p.ois ? 1 : 0) << "\n"
	  << "pro_eis=" << (p.eis ? 1 : 0) << "\n"
	  << "pro_flicker=" << p.flicker << "\n";
}
