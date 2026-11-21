#include "VideoReceiver.hpp"
#include "../shared/FrameShm.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>

static int InterruptCallback(void* opaque)
{
	return static_cast<VideoReceiver*>(opaque)->shouldInterrupt() ? 1 : 0;
}

static enum AVPixelFormat PickD3D11(AVCodecContext* ctx, const enum AVPixelFormat* fmts)
{
	for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
		if (*p == AV_PIX_FMT_D3D11) return *p;
	// GPU path not offered for this stream: let FFmpeg pick a CPU format.
	return avcodec_default_get_format(ctx, fmts);
}

static std::string AvErr(int code)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buf, sizeof(buf));
	return buf;
}

VideoReceiver::VideoReceiver()
{
	avformat_network_init();
}

VideoReceiver::~VideoReceiver()
{
	close();
	avformat_network_deinit();
}

void VideoReceiver::armTimeout(int ms)
{
	m_deadlineQpc = ocam::QpcNow() + (int64_t)ms * ocam::QpcFreq() / 1000;
}

bool VideoReceiver::shouldInterrupt() const
{
	if (m_abort && m_abort->load()) return true;
	return m_deadlineQpc != 0 && ocam::QpcNow() > m_deadlineQpc;
}

bool VideoReceiver::open(const std::string& url, const Options& opt, const std::atomic<bool>* abort, std::string& error)
{
	close();
	m_abort = abort;
	m_readTimeoutMs = opt.readTimeoutMs;

	m_fmt = avformat_alloc_context();
	if (!m_fmt)
	{
		error = "Out of memory.";
		return false;
	}
	m_fmt->interrupt_callback.callback = InterruptCallback;
	m_fmt->interrupt_callback.opaque = this;

	AVDictionary* o = nullptr;
	// The phone's RTSP server (libstreaming) only offers RTP over UDP.
	av_dict_set(&o, "rtsp_transport", "udp", 0);
	// Bigger socket buffer: Wi-Fi delivers in bursts, the default drops packets.
	av_dict_set(&o, "buffer_size", "2097152", 0);
	// Wait at most 100 ms for re-ordered packets (default 500 ms adds lag).
	av_dict_set(&o, "max_delay", "100000", 0);
	av_dict_set(&o, "reorder_queue_size", "64", 0);
	av_dict_set(&o, "fflags", "nobuffer+discardcorrupt", 0);
	av_dict_set(&o, "flags", "low_delay", 0);
	// The SDP already carries SPS/PPS, so a short probe is enough.
	av_dict_set(&o, "probesize", "131072", 0);
	av_dict_set(&o, "analyzeduration", "700000", 0);
#if LIBAVFORMAT_VERSION_MAJOR >= 59
	av_dict_set(&o, "timeout", "5000000", 0);
#else
	av_dict_set(&o, "stimeout", "5000000", 0);
#endif

	armTimeout(opt.openTimeoutMs);
	int rc = avformat_open_input(&m_fmt, url.c_str(), nullptr, &o);
	av_dict_free(&o);
	if (rc < 0)
	{
		m_fmt = nullptr; // freed by avformat_open_input on failure
		if (m_abort && m_abort->load()) error = "Cancelled.";
		else error = "The phone didn't send video. Is the OpenCamera app still open on the phone? (" + AvErr(rc) + ")";
		return false;
	}

	armTimeout(opt.openTimeoutMs);
	rc = avformat_find_stream_info(m_fmt, nullptr);
	if (rc < 0)
	{
		error = "Could not read the video format from the phone.";
		close();
		return false;
	}
	m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (m_stream < 0)
	{
		error = "The phone sent no video.";
		close();
		return false;
	}

	if (!openDecoder(opt.gpuDecode) && !(opt.gpuDecode && openDecoder(false)))
	{
		error = "Could not start the H.264 decoder.";
		close();
		return false;
	}

	m_frame = av_frame_alloc();
	m_latest = av_frame_alloc();
	m_swFrame = av_frame_alloc();
	m_pkt = av_packet_alloc();
	if (!m_frame || !m_latest || !m_swFrame || !m_pkt)
	{
		error = "Out of memory.";
		close();
		return false;
	}
	av_read_play(m_fmt);
	return true;
}

bool VideoReceiver::openDecoder(bool tryGpu)
{
	if (m_dec) avcodec_free_context(&m_dec);
	if (m_hwDevice) av_buffer_unref(&m_hwDevice);
	m_hwActive = false;

	const AVCodecParameters* par = m_fmt->streams[m_stream]->codecpar;
	const AVCodec* codec = avcodec_find_decoder(par->codec_id);
	if (!codec) return false;
	m_dec = avcodec_alloc_context3(codec);
	if (!m_dec) return false;
	if (avcodec_parameters_to_context(m_dec, par) < 0) return false;

	m_dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	m_dec->flags2 |= AV_CODEC_FLAG2_FAST;
	// Slice threads add no delay; frame threads would buffer whole frames.
	m_dec->thread_type = FF_THREAD_SLICE;
	m_dec->thread_count = 0;

	if (tryGpu && av_hwdevice_ctx_create(&m_hwDevice, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0)
	{
		m_dec->hw_device_ctx = av_buffer_ref(m_hwDevice);
		m_dec->get_format = PickD3D11;
		m_hwActive = true;
	}
	if (avcodec_open2(m_dec, codec, nullptr) < 0)
	{
		avcodec_free_context(&m_dec);
		if (m_hwDevice) av_buffer_unref(&m_hwDevice);
		m_hwActive = false;
		return false;
	}
	m_hwErrors = 0;
	return true;
}

void VideoReceiver::close()
{
	if (m_sws) sws_freeContext(m_sws);
	m_sws = nullptr;
	m_swsKey[2] = -1;
	if (m_pkt) av_packet_free(&m_pkt);
	if (m_frame) av_frame_free(&m_frame);
	if (m_latest) av_frame_free(&m_latest);
	if (m_swFrame) av_frame_free(&m_swFrame);
	if (m_dec) avcodec_free_context(&m_dec);
	if (m_hwDevice) av_buffer_unref(&m_hwDevice);
	if (m_fmt) avformat_close_input(&m_fmt);
	m_stream = -1;
	m_hwActive = false;
	m_width = m_height = 0;
	m_bytes = 0;
	m_deadlineQpc = 0;
}

int VideoReceiver::step(ocam::FrameWriter& out)
{
	if (!m_fmt || !m_dec) return -1;

	armTimeout(m_readTimeoutMs);
	int rc = av_read_frame(m_fmt, m_pkt);
	if (rc < 0)
	{
		if (rc == AVERROR(EAGAIN)) return 0;
		m_error = (m_abort && m_abort->load()) ? "Stopped." :
			(rc == AVERROR_EXIT ? "No video from the phone for a few seconds." : "Video stream ended (" + AvErr(rc) + ").");
		return -1;
	}
	if (m_pkt->stream_index != m_stream)
	{
		av_packet_unref(m_pkt);
		return 0;
	}
	m_bytes += m_pkt->size;

	const int64_t t0 = ocam::QpcNow();
	rc = avcodec_send_packet(m_dec, m_pkt);
	av_packet_unref(m_pkt);
	if (rc < 0 && rc != AVERROR(EAGAIN))
	{
		// Broken packets happen on Wi-Fi; only give up on the GPU path if it
		// keeps failing, then continue on the CPU.
		if (m_hwActive && ++m_hwErrors > 30)
			openDecoder(false);
		return 0;
	}

	// Drain everything the decoder has; only the newest picture is shown.
	bool got = false;
	for (;;)
	{
		rc = avcodec_receive_frame(m_dec, m_frame);
		if (rc < 0) break;
		av_frame_unref(m_latest);
		av_frame_move_ref(m_latest, m_frame);
		got = true;
	}
	if (!got) return 0;

	const bool ok = publish(m_latest, out);
	av_frame_unref(m_latest);
	m_lastProcessMs = ocam::QpcToMs(ocam::QpcNow() - t0);
	return ok ? 1 : 0;
}

bool VideoReceiver::publish(AVFrame* frame, ocam::FrameWriter& out)
{
	AVFrame* src = frame;
	if (frame->format == AV_PIX_FMT_D3D11)
	{
		av_frame_unref(m_swFrame);
		if (av_hwframe_transfer_data(m_swFrame, frame, 0) < 0)
		{
			if (++m_hwErrors > 30) openDecoder(false);
			return false;
		}
		// transfer_data copies pixels only; colour range/matrix live in the props.
		av_frame_copy_props(m_swFrame, frame);
		src = m_swFrame;
	}

	const int sw = src->width, sh = src->height;
	if (sw <= 0 || sh <= 0) return false;
	// Fit inside the shared-memory limit (1920x1080), keeping aspect ratio.
	int dw = sw, dh = sh;
	if (dw > (int)ocam::kMaxWidth || dh > (int)ocam::kMaxHeight)
	{
		const double s = std::min((double)ocam::kMaxWidth / dw, (double)ocam::kMaxHeight / dh);
		dw = std::max(2, (int)(dw * s) & ~1);
		dh = std::max(2, (int)(dh * s) & ~1);
	}

	const int srcFmt = src->format;
	const bool fullRange = src->color_range == AVCOL_RANGE_JPEG || srcFmt == AV_PIX_FMT_YUVJ420P;
	if (!m_sws || m_swsKey[0] != sw || m_swsKey[1] != sh || m_swsKey[2] != srcFmt || m_swsKey[3] != dw || m_swsKey[4] != dh || m_swsKey[5] != (int)fullRange)
	{
		if (m_sws) sws_freeContext(m_sws);
		const int flags = (dw == sw && dh == sh) ? SWS_POINT : SWS_BILINEAR;
		m_sws = sws_getContext(sw, sh, (AVPixelFormat)srcFmt, dw, dh, AV_PIX_FMT_BGR24, flags, nullptr, nullptr, nullptr);
		if (!m_sws) return false;
		const int* coef = sws_getCoefficients(sh >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601);
		sws_setColorspaceDetails(m_sws, coef, fullRange ? 1 : 0, coef, 1, 0, 1 << 16, 1 << 16);
		m_swsKey[0] = sw; m_swsKey[1] = sh; m_swsKey[2] = srcFmt; m_swsKey[3] = dw; m_swsKey[4] = dh; m_swsKey[5] = (int)fullRange;
	}

	uint32_t stride = 0;
	uint8_t* slot = out.begin((uint32_t)dw, (uint32_t)dh, &stride);
	if (!slot) return false;
	// Write bottom-up (DIB order) directly by starting at the last row with a
	// negative stride: no extra flip/copy pass.
	uint8_t* dst[4] = { slot + (size_t)(dh - 1) * stride, nullptr, nullptr, nullptr };
	int dstStride[4] = { -(int)stride, 0, 0, 0 };
	sws_scale(m_sws, src->data, src->linesize, 0, sh, dst, dstStride);
	out.commit();
	m_width = dw;
	m_height = dh;
	return true;
}
