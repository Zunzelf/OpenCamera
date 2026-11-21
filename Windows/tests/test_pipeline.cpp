// Headless test: VideoReceiver -> FrameWriter (shared memory) -> FrameReader.
// Usage: test_pipeline <video file> <gpu 0|1> <expect B> <expect G> <expect R>
#include "VideoReceiver.hpp"
#include "../shared/FrameShm.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cmath>

int main(int argc, char** argv)
{
	if (argc < 6) { printf("usage\n"); return 2; }
	const bool gpu = atoi(argv[2]) != 0;
	const int eb = atoi(argv[3]), eg = atoi(argv[4]), er = atoi(argv[5]);

	ocam::FrameWriter w;
	if (!w.open()) { printf("FAIL writer open\n"); return 1; }
	w.setState(ocam::STATE_LIVE);
	ocam::FrameReader r;
	if (!r.open()) { printf("FAIL reader open\n"); return 1; }

	std::atomic<bool> abort{ false };
	VideoReceiver rx;
	VideoReceiver::Options opt;
	opt.gpuDecode = gpu;
	std::string err;
	if (!rx.open(argv[1], opt, &abort, err)) { printf("FAIL open: %s\n", err.c_str()); return 1; }
	printf("opened, hw=%d\n", rx.hwActive() ? 1 : 0);

	int frames = 0, checked = 0, bad = 0, flipBad = 0;
	double procSum = 0, procMax = 0;
	const int64_t t0 = ocam::QpcNow();
	for (;;)
	{
		const int s = rx.step(w);
		if (s < 0) break;
		if (s == 0) continue;
		frames++;
		procSum += rx.lastProcessMs();
		if (rx.lastProcessMs() > procMax) procMax = rx.lastProcessMs();
		if (!r.live()) { printf("FAIL reader not live\n"); return 1; }
		r.readLatest([&](const uint8_t* d, uint32_t fw, uint32_t fh, uint32_t stride) {
			// centre pixel colour (BGR)
			const uint8_t* p = d + (size_t)(fh / 2) * stride + (fw / 2) * 3;
			if (abs(p[0] - eb) > 20 || abs(p[1] - eg) > 20 || abs(p[2] - er) > 20) bad++;
			// orientation: the test video has a white bar on the TOP rows. In a
			// bottom-up DIB the top image row is the LAST memory row.
			const uint8_t* top = d + (size_t)(fh - 2) * stride + (fw / 2) * 3;
			const uint8_t* bottom = d + (size_t)1 * stride + (fw / 2) * 3;
			if (!(top[0] > 200 && top[1] > 200 && top[2] > 200 && bottom[0] < 60 + eb)) flipBad++;
			checked++;
		});
	}
	const double wall = ocam::QpcToMs(ocam::QpcNow() - t0);
	printf("frames=%d checked=%d colourMismatch=%d orientationMismatch=%d size=%dx%d hw=%d avgProcess=%.2fms maxProcess=%.2fms wall=%.0fms counter=%lld\n",
		frames, checked, bad, flipBad, rx.width(), rx.height(), rx.hwActive() ? 1 : 0,
		frames ? procSum / frames : 0, procMax, wall, (long long)r.frameCounter());
	return (frames > 0 && bad == 0 && flipBad == 0) ? 0 : 1;
}
