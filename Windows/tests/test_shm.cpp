// Stress test for the lock-free triple buffer: a writer publishes frames as
// fast as possible (each frame filled with one byte value), readers copy the
// newest frame and verify it is not torn (all bytes equal).
#include "../shared/FrameShm.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main()
{
	ocam::FrameWriter w;
	if (!w.open()) { printf("FAIL open\n"); return 1; }
	w.setState(ocam::STATE_LIVE);
	std::atomic<bool> stop{ false };
	const uint32_t W = 1280, H = 720;

	std::thread writer([&]() {
		uint8_t v = 0;
		while (!stop)
		{
			uint32_t stride;
			uint8_t* p = w.begin(W, H, &stride);
			memset(p, ++v, (size_t)stride * H);
			w.commit();
		}
	});

	std::atomic<long> reads{ 0 }, torn{ 0 }, failed{ 0 };
	std::vector<std::thread> readers;
	for (int i = 0; i < 3; i++)
		readers.emplace_back([&]() {
			ocam::FrameReader r;
			while (!r.open()) {}
			std::vector<uint8_t> buf(ocam::kSlotBytes);
			while (!stop)
			{
				uint32_t n = 0;
				const bool ok = r.readLatest([&](const uint8_t* d, uint32_t, uint32_t h, uint32_t stride) {
					n = stride * h;
					memcpy(buf.data(), d, n);
				});
				if (!ok) { failed++; continue; }
				reads++;
				for (uint32_t k = 1; k < n; k += 4093)
					if (buf[k] != buf[0]) { torn++; break; }
			}
		});

	std::this_thread::sleep_for(std::chrono::seconds(3));
	stop = true;
	writer.join();
	for (auto& t : readers) t.join();
	printf("frames written=%lld reads=%ld torn=%ld retries-exhausted=%ld\n",
		(long long)w.header()->frameCounter, reads.load(), torn.load(), failed.load());
	return torn == 0 ? 0 : 1;
}
