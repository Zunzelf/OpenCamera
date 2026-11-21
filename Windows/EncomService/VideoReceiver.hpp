#pragma once
// Pulls the phone's H.264 RTSP stream, decodes it (GPU via D3D11VA when
// available, CPU otherwise) and converts each picture straight into the
// shared-memory slot the virtual camera reads from (one conversion, no
// intermediate buffers).

#include <atomic>
#include <cstdint>
#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct SwsContext;

namespace ocam { class FrameWriter; }

class VideoReceiver
{
public:
	struct Options
	{
		bool gpuDecode = false;
		bool tcp = false; // RTP inside the RTSP connection (USB cable via adb)
		int openTimeoutMs = 8000;
		int readTimeoutMs = 4000;
	};

	VideoReceiver();
	~VideoReceiver();

	// Opens the stream. `abort` is polled during blocking network calls so
	// the caller can cancel instantly. On failure, `error` is a friendly text.
	bool open(const std::string& url, const Options& opt, const std::atomic<bool>* abort, std::string& error);
	void close();

	// Reads one packet and, if it completes a picture, writes the newest
	// picture to `out`. Returns 1 if a frame was published, 0 if not yet,
	// -1 on end of stream / error (see lastError()).
	int step(ocam::FrameWriter& out);

	// Turn the picture clockwise by 0/90/180/270 degrees. With `fill`, a
	// picture that ends up taller than 16:9 is cropped to 16:9 instead of
	// getting black bars in video apps. Takes effect on the next frame.
	void setTransform(int rotation, bool fill) { m_rot = rotation; m_fill = fill; }
	int appliedRotation() const { return m_appliedRot; }
	float cropTop() const { return m_cropTop; }
	float cropHeight() const { return m_cropHeight; }
	// Brightness of the last frame (before rotation): 64-bin histogram of
	// luma, normalised so the tallest bin is 1, and the mean 0..255.
	const float* histogram() const { return m_hist; }
	float meanLuma() const { return m_meanLuma; }

	bool hwActive() const { return m_hwActive; }
	int width() const { return m_width; }
	int height() const { return m_height; }
	int64_t bytesReceived() const { return m_bytes; }
	double lastProcessMs() const { return m_lastProcessMs; }
	const std::string& lastError() const { return m_error; }

	// Used by the FFmpeg interrupt callback.
	bool shouldInterrupt() const;

private:
	bool openDecoder(bool tryGpu);
	bool publish(AVFrame* frame, ocam::FrameWriter& out);
	AVFrame* rotate(AVFrame* src, int rot);
	void measureLuma(const AVFrame* src);

	AVFormatContext* m_fmt = nullptr;
	AVCodecContext* m_dec = nullptr;
	AVBufferRef* m_hwDevice = nullptr;
	AVFrame* m_frame = nullptr;
	AVFrame* m_latest = nullptr;
	AVFrame* m_swFrame = nullptr;
	AVFrame* m_rotFrame = nullptr;
	std::atomic<int> m_rot{ 0 };
	std::atomic<bool> m_fill{ false };
	int m_appliedRot = 0;
	float m_cropTop = 0, m_cropHeight = 1;
	float m_hist[64] = {};
	float m_meanLuma = 0;
	AVPacket* m_pkt = nullptr;
	SwsContext* m_sws = nullptr;
	int m_swsKey[6] = { 0, 0, -1, 0, 0, 0 };
	int m_stream = -1;
	bool m_hwActive = false;
	int m_hwErrors = 0;
	int m_width = 0, m_height = 0;
	int64_t m_bytes = 0;
	double m_lastProcessMs = 0;
	std::string m_error;

	const std::atomic<bool>* m_abort = nullptr;
	int64_t m_deadlineQpc = 0;
	int m_readTimeoutMs = 4000;
	void armTimeout(int ms);
};
