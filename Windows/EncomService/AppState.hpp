#pragma once
// State shared between the UI thread and the phone/video threads.
// Simple values are atomics; strings are guarded by a mutex.

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

enum class Phase
{
	Waiting,      // listening, no phone yet
	AskApproval,  // a new phone wants to connect, user must decide
	Connecting,   // phone accepted, opening video
	Live,         // frames flowing
	Reconnecting, // video dropped, retrying while the phone is still there
	Error,        // something needs the user's attention
};

enum class LogLevel { Info, Warning, Error };

struct LogLine
{
	LogLevel level;
	std::string text;
	std::string time;
};

struct QualityPreset
{
	const char* label;
	int width;
	int height;
	int fps;
	int kbps;
};

static const QualityPreset kQualityPresets[] = {
	{ "480p", 640, 480, 30, 2000 },
	{ "720p", 1280, 720, 30, 4000 },
	{ "1080p", 1920, 1080, 30, 8000 },
};
static const int kQualityPresetCount = 3;

// "Pro" camera controls the PC asks the phone for (Sony-style). 0 = Auto.
struct ProSettings
{
	int iso = 0;           // 0 = auto
	int64_t expNs = 0;     // shutter in nanoseconds, 0 = auto
	int ev = 0;            // exposure compensation in the phone's steps
	int wb = 0;            // 0 auto, 1 daylight, 2 cloudy, 3 shade, 4 tungsten, 5 fluorescent, 6 Kelvin
	int kelvin = 5500;
	bool manualFocus = false;
	float focus = 0.0f;    // diopters (1/m), 0 = infinity
	bool aeLock = false;
	bool awbLock = false;
	bool afLock = false;
	float zoom = 1.0f;
	bool torch = false;
	bool ois = true;
	bool eis = false;
	int flicker = 0;       // 0 auto, 1 50 Hz, 2 60 Hz, 3 off
};

// What the phone's current camera supports (sent by the phone).
struct PhoneCaps
{
	bool valid = false;
	bool front = false, manual = false, kelvin = false, torch = false, ois = false, eis = false, point = false;
	int isoMin = 100, isoMax = 3200;
	int64_t expMin = 100000, expMax = 100000000;
	int evMin = 0, evMax = 0;
	float evStep = 1.0f / 3.0f;
	float minFocus = 0;    // diopters; 0 = fixed focus
	float zoomMin = 1, zoomMax = 1;
	std::vector<std::pair<int, std::vector<int>>> fps; // stream height -> frame rates
};

// Live readings from the phone, ~4 per second.
struct PhoneStatus
{
	bool valid = false;
	int iso = 0;
	int64_t expNs = 0;
	float focus = -1;
	int afState = -1, aeState = -1;
	bool torch = false;
	int battery = -1, temperature = -1;
	bool charging = false;
	int rotation = -1;     // clockwise degrees to turn the picture upright, -1 unknown
	int level = 0;         // tilt from that orientation, degrees
};

class AppState
{
public:
	// ---- user settings (persisted)
	std::atomic<int> quality{ 1 };
	std::atomic<bool> frontCamera{ true };
	std::atomic<bool> mirror{ false };
	std::atomic<bool> gpuDecode{ false }; // CPU is faster end-to-end for <=1080p (measured)
	std::atomic<int> frameRate{ 30 };
	std::atomic<int> rotationMode{ 0 };  // 0 auto (from the phone), 1..4 = 0/90/180/270 deg
	std::atomic<bool> fillFrame{ false }; // crop a sideways picture to fill 16:9 instead of black bars
	std::atomic<bool> showGrid{ false };
	std::atomic<bool> showHistogram{ true };
	std::atomic<bool> showZebra{ false };
	std::atomic<bool> showPeaking{ false };
	std::atomic<bool> showLevel{ true };

	// ---- requests from UI to the phone link
	std::atomic<bool> requestRestartVideo{ false };
	std::atomic<bool> requestDisconnect{ false };
	std::atomic<int> approvalAnswer{ 0 }; // 0 none, 1 allow, 2 block
	std::atomic<bool> approvalRemember{ true };
	std::atomic<bool> proDirty{ false };   // send pro settings to the phone
	std::atomic<bool> pointDirty{ false }; // send focus point to the phone

	// ---- live status (written by worker threads)
	std::atomic<Phase> phase{ Phase::Waiting };
	std::atomic<float> fps{ 0 };
	std::atomic<float> kbps{ 0 };
	std::atomic<float> decodeMs{ 0 };
	std::atomic<int> videoWidth{ 0 };
	std::atomic<int> videoHeight{ 0 };
	std::atomic<bool> hwDecodeActive{ false };
	std::atomic<bool> quitting{ false };
	std::atomic<int> appliedRotation{ 0 };  // what the decoder is applying now
	std::atomic<float> cropTop{ 0 };         // fill-frame crop, as fractions of the turned picture
	std::atomic<float> cropHeight{ 1 };
	std::atomic<bool> phoneTalksBack{ false };
	// How the phone reaches this PC (see LinkKind in PhoneLink.cpp).
	std::atomic<int> linkKind{ 0 }; // 0 unknown, 1 Wi-Fi, 2 USB cable, 3 network cable, 4 this PC

	ProSettings getPro();
	void setPro(const ProSettings& p, bool sendToPhone = true);
	PhoneCaps getCaps();
	void setCaps(const PhoneCaps& c);
	PhoneStatus getStatus();
	void setStatus(const PhoneStatus& s);
	void clearPhoneInfo();
	void setPoint(float x, float y);         // stream coordinates 0..1, or -1 to clear
	void getPoint(float& x, float& y);
	void setLuma(const float* hist64, float mean);
	float getLuma(float* hist64);            // returns mean 0..255

	void setMessage(const std::string& headline, const std::string& detail);
	void getMessage(std::string& headline, std::string& detail);

	void setPhone(const std::string& name, const std::string& address);
	void getPhone(std::string& name, std::string& address);

	void log(LogLevel level, const std::string& text);
	std::vector<LogLine> logSnapshot();
	void clearLog();

	void loadSettings();
	void saveSettings();

	static std::string dataDir();

private:
	std::mutex m_lock;
	std::string m_headline, m_detail;
	std::string m_phoneName, m_phoneAddress;
	std::deque<LogLine> m_log;
	ProSettings m_pro;
	PhoneCaps m_caps;
	PhoneStatus m_status;
	float m_pointX = -1, m_pointY = -1;
	std::array<float, 64> m_hist{};
	float m_meanLuma = 0;
};

extern AppState g_app;
