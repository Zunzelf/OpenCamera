#pragma once
// Frame transport between OpenCamera (writer, EncomService.exe) and the
// DirectShow virtual camera (reader, EncomCamera*.dll loaded into Zoom, Chrome, ...).
//
// Layout: one named file mapping = Header + kSlots frame slots.
// Each frame is stored as a bottom-up BGR24 DIB, so the reader can hand it
// to DirectShow with a single memcpy.
//
// Synchronisation is lock-free (no mutex):
//  * the writer fills a slot that is not the newest one, guarded by a per-slot
//    sequence counter (odd = being written, even = stable), then publishes the
//    slot index in `latest` and bumps `frameCounter`;
//  * readers copy `latest`, and retry if the slot's sequence changed meanwhile.
// Readers therefore need read-only access, never block the writer, and always
// get the newest complete frame (late frames are dropped, not queued).
//
// The objects live in the per-session "Local\" namespace, which does not
// require administrator rights (the old "Global\" names did).

#include <Windows.h>
#include <stdint.h>

namespace ocam
{
static const wchar_t kShmName[] = L"Local\\OpenCameraFrames_v2";

static const uint32_t kMagic = 0x324D434Fu; // "OCM2"
static const uint32_t kVersion = 2;
static const uint32_t kMaxWidth = 1920;
static const uint32_t kMaxHeight = 1080;
static const uint32_t kSlots = 3;
static const uint32_t kSlotBytes = kMaxWidth * kMaxHeight * 3;

// Writer is considered gone if it has not updated its heartbeat for this long.
static const int64_t kHeartbeatTimeoutMs = 2000;

enum StreamState : uint32_t
{
	STATE_IDLE = 0,       // app running, no phone
	STATE_CONNECTING = 1, // phone accepted, waiting for video
	STATE_LIVE = 2,       // frames flowing
};

struct alignas(64) SlotHeader
{
	volatile LONG64 seq;  // odd while being written
	uint32_t width;
	uint32_t height;
	uint32_t stride;      // bytes per row (DWORD aligned), rows stored bottom-up
	uint32_t reserved;
	int64_t qpcCaptured;  // QueryPerformanceCounter when the decoded frame was published
};

struct alignas(64) Header
{
	uint32_t magic;
	uint32_t version;
	uint32_t slotBytes;
	uint32_t slots;
	volatile LONG latest;        // newest complete slot, -1 if none
	volatile LONG state;         // StreamState
	volatile LONG mirror;        // 1 = readers flip horizontally while copying
	volatile LONG reserved0;
	volatile LONG64 frameCounter;
	volatile LONG64 heartbeatQpc; // writer's last sign of life
	SlotHeader slot[kSlots];
};

inline size_t MappingSize()
{
	return sizeof(Header) + (size_t)kSlots * kSlotBytes;
}

inline uint8_t* SlotData(Header* h, uint32_t index)
{
	return reinterpret_cast<uint8_t*>(h) + sizeof(Header) + (size_t)index * kSlotBytes;
}

inline const uint8_t* SlotData(const Header* h, uint32_t index)
{
	return reinterpret_cast<const uint8_t*>(h) + sizeof(Header) + (size_t)index * kSlotBytes;
}

inline uint32_t DibStride(uint32_t width)
{
	return (width * 3u + 3u) & ~3u;
}

inline int64_t QpcNow()
{
	LARGE_INTEGER v;
	QueryPerformanceCounter(&v);
	return v.QuadPart;
}

inline int64_t QpcFreq()
{
	static int64_t f = 0;
	if (!f)
	{
		LARGE_INTEGER v;
		QueryPerformanceFrequency(&v);
		f = v.QuadPart;
	}
	return f;
}

inline double QpcToMs(int64_t ticks)
{
	return (double)ticks * 1000.0 / (double)QpcFreq();
}

// ---------------------------------------------------------------- writer side
class FrameWriter
{
public:
	~FrameWriter() { close(); }

	bool open()
	{
		if (m_hdr) return true;
		// Null DACL: any process in this session (any integrity level that can
		// read medium objects) may open it.
		SECURITY_DESCRIPTOR sd;
		InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
		SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
		SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
		const uint64_t size = MappingSize();
		m_map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
			(DWORD)(size >> 32), (DWORD)(size & 0xFFFFFFFFu), kShmName);
		if (!m_map) return false;
		m_hdr = (Header*)MapViewOfFile(m_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		if (!m_hdr)
		{
			CloseHandle(m_map);
			m_map = NULL;
			return false;
		}
		ZeroMemory(m_hdr, sizeof(Header));
		m_hdr->slotBytes = kSlotBytes;
		m_hdr->slots = kSlots;
		m_hdr->latest = -1;
		m_hdr->version = kVersion;
		MemoryBarrier();
		m_hdr->magic = kMagic;
		heartbeat();
		return true;
	}

	void close()
	{
		if (m_hdr)
		{
			setState(STATE_IDLE);
			InterlockedExchange64(&m_hdr->heartbeatQpc, 0);
			UnmapViewOfFile(m_hdr);
			m_hdr = nullptr;
		}
		if (m_map)
		{
			CloseHandle(m_map);
			m_map = NULL;
		}
	}

	bool isOpen() const { return m_hdr != nullptr; }

	void heartbeat()
	{
		if (m_hdr) InterlockedExchange64(&m_hdr->heartbeatQpc, QpcNow());
	}

	void setState(StreamState s)
	{
		if (!m_hdr) return;
		InterlockedExchange(&m_hdr->state, (LONG)s);
		if (s != STATE_LIVE) InterlockedExchange(&m_hdr->latest, -1);
	}

	void setMirror(bool on)
	{
		if (m_hdr) InterlockedExchange(&m_hdr->mirror, on ? 1 : 0);
	}

	// Reserve a slot for a width x height frame. Returns a pointer to the
	// first byte of the slot, or nullptr if the size is unsupported.
	uint8_t* begin(uint32_t width, uint32_t height, uint32_t* strideOut)
	{
		if (!m_hdr || width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight)
			return nullptr;
		const LONG latest = m_hdr->latest;
		// Next slot that is neither the published one nor the one we used last.
		uint32_t idx = (m_lastWritten + 1) % kSlots;
		if ((LONG)idx == latest) idx = (idx + 1) % kSlots;
		m_writing = idx;
		SlotHeader& s = m_hdr->slot[idx];
		InterlockedIncrement64(&s.seq); // -> odd
		s.width = width;
		s.height = height;
		s.stride = DibStride(width);
		*strideOut = s.stride;
		return SlotData(m_hdr, idx);
	}

	void commit()
	{
		if (!m_hdr) return;
		SlotHeader& s = m_hdr->slot[m_writing];
		s.qpcCaptured = QpcNow();
		InterlockedIncrement64(&s.seq); // -> even, full barrier
		InterlockedExchange(&m_hdr->latest, (LONG)m_writing);
		InterlockedIncrement64(&m_hdr->frameCounter);
		m_lastWritten = m_writing;
		heartbeat();
	}

	const Header* header() const { return m_hdr; }

private:
	HANDLE m_map = NULL;
	Header* m_hdr = nullptr;
	uint32_t m_writing = 0;
	uint32_t m_lastWritten = 0;
};

// ---------------------------------------------------------------- reader side
class FrameReader
{
public:
	~FrameReader() { close(); }

	bool open()
	{
		if (m_hdr) return true;
		m_map = OpenFileMappingW(FILE_MAP_READ, FALSE, kShmName);
		if (!m_map) return false;
		m_hdr = (const Header*)MapViewOfFile(m_map, FILE_MAP_READ, 0, 0, 0);
		if (!m_hdr || m_hdr->magic != kMagic || m_hdr->version != kVersion)
		{
			close();
			return false;
		}
		return true;
	}

	void close()
	{
		if (m_hdr) UnmapViewOfFile(m_hdr);
		m_hdr = nullptr;
		if (m_map) CloseHandle(m_map);
		m_map = NULL;
	}

	bool isOpen() const { return m_hdr != nullptr; }

	// True if the writer process is alive (recent heartbeat).
	bool writerAlive() const
	{
		if (!m_hdr) return false;
		const int64_t hb = m_hdr->heartbeatQpc;
		return hb != 0 && QpcToMs(QpcNow() - hb) < (double)kHeartbeatTimeoutMs;
	}

	bool live() const
	{
		return writerAlive() && m_hdr->state == STATE_LIVE && m_hdr->latest >= 0;
	}

	int64_t frameCounter() const { return m_hdr ? m_hdr->frameCounter : 0; }
	bool mirror() const { return m_hdr && m_hdr->mirror != 0; }
	StreamState state() const { return m_hdr ? (StreamState)m_hdr->state : STATE_IDLE; }

	// Calls fn(const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride)
	// with a consistent snapshot of the newest frame. `fn` must only copy.
	// Returns false if no complete frame could be read.
	template <class Fn>
	bool readLatest(Fn&& fn) const
	{
		if (!m_hdr) return false;
		for (int attempt = 0; attempt < 4; ++attempt)
		{
			const LONG idx = m_hdr->latest;
			if (idx < 0 || idx >= (LONG)kSlots) return false;
			const SlotHeader& s = m_hdr->slot[idx];
			const LONG64 seq0 = s.seq;
			if (seq0 & 1) continue; // being rewritten, take the new latest
			MemoryBarrier();
			const uint32_t w = s.width, h = s.height, stride = s.stride;
			if (w == 0 || h == 0 || w > kMaxWidth || h > kMaxHeight || stride * h > kSlotBytes)
				return false;
			fn(SlotData(m_hdr, (uint32_t)idx), w, h, stride);
			MemoryBarrier();
			if (s.seq == seq0) return true;
		}
		return false;
	}

private:
	HANDLE m_map = NULL;
	const Header* m_hdr = nullptr;
};
} // namespace ocam
