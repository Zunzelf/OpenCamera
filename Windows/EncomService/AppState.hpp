#pragma once
// State shared between the UI thread and the phone/video threads.
// Simple values are atomics; strings are guarded by a mutex.

#include <atomic>
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

class AppState
{
public:
	// ---- user settings (persisted)
	std::atomic<int> quality{ 1 };
	std::atomic<bool> frontCamera{ true };
	std::atomic<bool> mirror{ false };
	std::atomic<bool> gpuDecode{ false }; // CPU is faster end-to-end for <=1080p (measured)

	// ---- requests from UI to the phone link
	std::atomic<bool> requestRestartVideo{ false };
	std::atomic<bool> requestDisconnect{ false };
	std::atomic<int> approvalAnswer{ 0 }; // 0 none, 1 allow, 2 block
	std::atomic<bool> approvalRemember{ true };

	// ---- live status (written by worker threads)
	std::atomic<Phase> phase{ Phase::Waiting };
	std::atomic<float> fps{ 0 };
	std::atomic<float> kbps{ 0 };
	std::atomic<float> decodeMs{ 0 };
	std::atomic<int> videoWidth{ 0 };
	std::atomic<int> videoHeight{ 0 };
	std::atomic<bool> hwDecodeActive{ false };
	std::atomic<bool> quitting{ false };

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
};

extern AppState g_app;
