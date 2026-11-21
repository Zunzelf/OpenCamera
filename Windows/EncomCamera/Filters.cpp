#pragma warning(disable:4244)
#pragma warning(disable:4711)

#include <Windows.h>

#include <streams.h>
#include <stdio.h>
#include <olectl.h>
#include <dvdmedia.h>
#include <string.h>
#include "filters.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

//////////////////////////////////////////////////////////////////////////
//  CVCam is the source filter which masquerades as a capture device
//////////////////////////////////////////////////////////////////////////
CUnknown * WINAPI CVCam::CreateInstance(LPUNKNOWN lpunk, HRESULT *phr)
{
    ASSERT(phr);
    CUnknown *punk = new CVCam(lpunk, phr);
    return punk;
}

CVCam::CVCam(LPUNKNOWN lpunk, HRESULT *phr) :
    CSource(NAME("OpenCamera"), lpunk, CLSID_VirtualCam)
{
    ASSERT(phr);
    CAutoLock cAutoLock(&m_cStateLock);
    m_paStreams = (CSourceStream **) new CVCamStream*[1];
    m_paStreams[0] = new CVCamStream(phr, this, L"Capture");
}

HRESULT CVCam::QueryInterface(REFIID riid, void **ppv)
{
    // Forward request for IAMStreamConfig & IKsPropertySet to the pin
    if(riid == _uuidof(IAMStreamConfig) || riid == _uuidof(IKsPropertySet))
        return m_paStreams[0]->QueryInterface(riid, ppv);
    else
        return CSource::QueryInterface(riid, ppv);
}

//////////////////////////////////////////////////////////////////////////
// helpers
//////////////////////////////////////////////////////////////////////////
static void FillVideoInfo(VIDEOINFOHEADER* pvi, int w, int h)
{
    ZeroMemory(pvi, sizeof(VIDEOINFOHEADER));
    pvi->bmiHeader.biCompression = BI_RGB;
    pvi->bmiHeader.biBitCount = 24;
    pvi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pvi->bmiHeader.biWidth = w;
    pvi->bmiHeader.biHeight = h; // positive = bottom-up DIB
    pvi->bmiHeader.biPlanes = 1;
    pvi->bmiHeader.biSizeImage = GetBitmapSize(&pvi->bmiHeader);
    pvi->AvgTimePerFrame = UNITS / kVCamFps;
    pvi->dwBitRate = (DWORD)(pvi->bmiHeader.biSizeImage * 8u * kVCamFps);
    SetRectEmpty(&(pvi->rcSource));
    SetRectEmpty(&(pvi->rcTarget));
}

static void FillMediaType(CMediaType* pmt, int w, int h)
{
    DECLARE_PTR(VIDEOINFOHEADER, pvi, pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER)));
    FillVideoInfo(pvi, w, h);
    pmt->SetType(&MEDIATYPE_Video);
    pmt->SetFormatType(&FORMAT_VideoInfo);
    pmt->SetTemporalCompression(FALSE);
    pmt->SetSubtype(&MEDIASUBTYPE_RGB24);
    pmt->SetSampleSize(pvi->bmiHeader.biSizeImage);
}

static bool IsOurFormat(const AM_MEDIA_TYPE* mt)
{
    if (!mt || mt->majortype != MEDIATYPE_Video || mt->subtype != MEDIASUBTYPE_RGB24)
        return false;
    if (mt->formattype != FORMAT_VideoInfo || mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat)
        return false;
    const VIDEOINFOHEADER* pvi = (const VIDEOINFOHEADER*)mt->pbFormat;
    if (pvi->bmiHeader.biBitCount != 24 || pvi->bmiHeader.biCompression != BI_RGB)
        return false;
    for (int i = 0; i < kVCamFormatCount; i++)
        if (pvi->bmiHeader.biWidth == kVCamFormats[i].width && pvi->bmiHeader.biHeight == kVCamFormats[i].height)
            return true;
    return false;
}

static int64_t QpcFrom100ns(REFERENCE_TIME t)
{
    return (int64_t)((double)t * (double)ocam::QpcFreq() / (double)UNITS);
}

static REFERENCE_TIME QpcTo100ns(int64_t ticks)
{
    return (REFERENCE_TIME)((double)ticks * (double)UNITS / (double)ocam::QpcFreq());
}

//////////////////////////////////////////////////////////////////////////
// CVCamStream: the single output pin
//////////////////////////////////////////////////////////////////////////
CVCamStream::CVCamStream(HRESULT *phr, CVCam *pParent, LPCWSTR pPinName) :
    CSourceStream(NAME("OpenCamera"), phr, pParent, pPinName), m_pParent(pParent)
{
    GetMediaType(0, &m_mt);
}

CVCamStream::~CVCamStream()
{
    if (m_timer) CloseHandle(m_timer);
}

HRESULT CVCamStream::QueryInterface(REFIID riid, void **ppv)
{
    if(riid == _uuidof(IAMStreamConfig))
        *ppv = (IAMStreamConfig*)this;
    else if(riid == _uuidof(IKsPropertySet))
        *ppv = (IKsPropertySet*)this;
    else
        return CSourceStream::QueryInterface(riid, ppv);

    AddRef();
    return S_OK;
}

void CVCamStream::sleepShort()
{
    // ~1 ms wait without changing the system-wide timer resolution.
    if (m_timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -10000; // 1 ms, relative
        if (SetWaitableTimer(m_timer, &due, 0, NULL, NULL, FALSE))
        {
            WaitForSingleObject(m_timer, 20);
            return;
        }
    }
    Sleep(1);
}

// Block until the app has published a newer frame, or the deadline passes
// (then the previous picture is repeated so the consumer never stalls).
void CVCamStream::waitForFrame(int64_t deadlineQpc)
{
    for (;;)
    {
        const int64_t now = ocam::QpcNow();
        if (!m_reader.isOpen() && ocam::QpcToMs(now - m_lastOpenTryQpc) > 500.0)
        {
            m_lastOpenTryQpc = now;
            m_reader.open();
        }
        if (m_reader.live() && m_reader.frameCounter() != m_lastFrameCounter)
            return;
        if (now >= deadlineQpc)
            return;
        sleepShort();
    }
}

// Nearest-neighbour blit of a bottom-up BGR24 image into a bottom-up BGR24
// buffer, letterboxed to keep the aspect ratio, optionally mirrored.
static void BlitScaled(const uint8_t* src, int sw, int sh, int sstride,
                       uint8_t* dst, int dw, int dh, int dstride,
                       bool mirror, std::vector<int>& xmap, int key[4])
{
    if (sw == dw && sh == dh && !mirror)
    {
        if (sstride == dstride)
            memcpy(dst, src, (size_t)dstride * dh);
        else
            for (int y = 0; y < dh; y++)
                memcpy(dst + (size_t)y * dstride, src + (size_t)y * sstride, (size_t)dw * 3);
        return;
    }
    // fit
    int tw = dw, th = (int)((int64_t)dw * sh / sw);
    if (th > dh) { th = dh; tw = (int)((int64_t)dh * sw / sh); }
    const int ox = (dw - tw) / 2, oy = (dh - th) / 2;
    if (tw != dw || th != dh)
        memset(dst, 0, (size_t)dstride * dh);

    if (key[0] != sw || key[1] != tw || key[2] != (mirror ? 1 : 0) || key[3] != sh)
    {
        xmap.resize(tw);
        for (int x = 0; x < tw; x++)
        {
            int sx = (int)(((int64_t)x * sw) / tw);
            if (mirror) sx = sw - 1 - sx;
            xmap[x] = sx * 3;
        }
        key[0] = sw; key[1] = tw; key[2] = mirror ? 1 : 0; key[3] = sh;
    }
    const int* xm = xmap.data();
    for (int y = 0; y < th; y++)
    {
        const int sy = (int)(((int64_t)y * sh) / th);
        const uint8_t* srow = src + (size_t)sy * sstride;
        uint8_t* drow = dst + (size_t)(y + oy) * dstride + (size_t)ox * 3;
        for (int x = 0; x < tw; x++)
        {
            const uint8_t* p = srow + xm[x];
            drow[0] = p[0]; drow[1] = p[1]; drow[2] = p[2];
            drow += 3;
        }
    }
}

// A calm "waiting" picture instead of a black frame, so people know the
// camera works and what to do next. Rendered once per size with GDI.
const std::vector<uint8_t>& CVCamStream::placeholder(int w, int h, bool appRunning)
{
    const int msg = appRunning ? 1 : 0;
    if (m_placeholderKey[0] == w && m_placeholderKey[1] == h && m_placeholderKey[2] == msg && !m_placeholder.empty())
        return m_placeholder;

    const int stride = (int)ocam::DibStride(w);
    m_placeholder.assign((size_t)stride * h, 0);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (dc && bmp && bits)
    {
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT rc = { 0, 0, w, h };
        HBRUSH bg = CreateSolidBrush(RGB(22, 24, 29));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        // camera glyph: rounded body + lens
        const int cx = w / 2, cy = h / 2 - h / 10, u = h / 18;
        HBRUSH body = CreateSolidBrush(RGB(64, 70, 82));
        HPEN nopen = (HPEN)GetStockObject(NULL_PEN);
        SelectObject(dc, nopen);
        SelectObject(dc, body);
        RoundRect(dc, cx - 3 * u, cy - 2 * u, cx + 3 * u, cy + 2 * u, u, u);
        DeleteObject(body);
        HBRUSH lens = CreateSolidBrush(appRunning ? RGB(94, 160, 255) : RGB(120, 126, 138));
        SelectObject(dc, lens);
        Ellipse(dc, cx - u, cy - u, cx + u, cy + u);
        DeleteObject(lens);

        SetBkMode(dc, TRANSPARENT);
        HFONT fontBig = CreateFontW(-(h / 16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT fontSmall = CreateFontW(-(h / 30), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        RECT t1 = { 0, cy + 3 * u, w, cy + 3 * u + h / 10 };
        SelectObject(dc, fontBig);
        SetTextColor(dc, RGB(236, 238, 242));
        DrawTextW(dc, L"OpenCamera", -1, &t1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        RECT t2 = { 0, t1.bottom, w, t1.bottom + h / 14 };
        SelectObject(dc, fontSmall);
        SetTextColor(dc, RGB(150, 156, 168));
        DrawTextW(dc, appRunning ? L"Waiting for your phone\x2026  Open OpenCamera on the phone and tap this PC."
                                 : L"Start the OpenCamera app on this PC to use your phone as a webcam.",
                  -1, &t2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, old);
        DeleteObject(fontBig);
        DeleteObject(fontSmall);
        GdiFlush();
        memcpy(m_placeholder.data(), bits, m_placeholder.size());
    }
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    m_placeholderKey[0] = w;
    m_placeholderKey[1] = h;
    m_placeholderKey[2] = msg;
    return m_placeholder;
}

HRESULT CVCamStream::FillBuffer(IMediaSample *pms)
{
    const VIDEOINFOHEADER* vih = (const VIDEOINFOHEADER*)m_mt.Format();
    const REFERENCE_TIME interval = vih->AvgTimePerFrame > 0 ? vih->AvgTimePerFrame : UNITS / kVCamFps;
    const int dw = vih->bmiHeader.biWidth;
    const int dh = abs(vih->bmiHeader.biHeight);
    const int dstride = (int)ocam::DibStride(dw);

    // Pace delivery: wait for a new frame, but never longer than 1.5 frame
    // intervals (or immediately for the very first sample).
    int64_t deadline = m_lastDeliverQpc == 0 ? 0 : m_lastDeliverQpc + QpcFrom100ns(interval * 3 / 2);
    waitForFrame(deadline);

    BYTE *pData = nullptr;
    pms->GetPointer(&pData);
    const long lDataLen = pms->GetSize();
    if (!pData || lDataLen < dstride * dh)
        return E_FAIL;

    bool delivered = false;
    if (m_reader.live())
    {
        // Always copy the newest frame (also when repeating: the pooled sample
        // buffer may hold an older picture). If the writer overwrote the slot
        // mid-copy, try again shortly; it only writes for a few ms per frame.
        const bool mirror = m_reader.mirror();
        for (int attempt = 0; attempt < 3 && !delivered; attempt++)
        {
            if (attempt) sleepShort();
            const int64_t counter = m_reader.frameCounter();
            delivered = m_reader.readLatest([&](const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t sstride) {
                BlitScaled(src, (int)sw, (int)sh, (int)sstride, pData, dw, dh, dstride, mirror, m_xmap, m_xmapKey);
            });
            if (delivered) m_lastFrameCounter = counter;
        }
    }
    if (!delivered)
    {
        const std::vector<uint8_t>& ph = placeholder(dw, dh, m_reader.writerAlive());
        memcpy(pData, ph.data(), ph.size() < (size_t)lDataLen ? ph.size() : (size_t)lDataLen);
    }

    const int64_t now = ocam::QpcNow();
    if (m_streamStartQpc == 0) m_streamStartQpc = now;
    REFERENCE_TIME rtStart = QpcTo100ns(now - m_streamStartQpc);
    REFERENCE_TIME rtStop = rtStart + interval;
    pms->SetTime(&rtStart, &rtStop);
    pms->SetSyncPoint(TRUE);
    pms->SetActualDataLength(dstride * dh);
    m_lastDeliverQpc = now;
    return S_OK;
}

STDMETHODIMP CVCamStream::Notify(IBaseFilter * pSender, Quality q)
{
    return E_NOTIMPL;
}

HRESULT CVCamStream::SetMediaType(const CMediaType *pmt)
{
    return CSourceStream::SetMediaType(pmt);
}

HRESULT CVCamStream::GetMediaType(int iPosition, CMediaType *pmt)
{
    if (iPosition < 0) return E_INVALIDARG;
    if (iPosition >= kVCamFormatCount) return VFW_S_NO_MORE_ITEMS;
    FillMediaType(pmt, kVCamFormats[iPosition].width, kVCamFormats[iPosition].height);
    return NOERROR;
}

HRESULT CVCamStream::CheckMediaType(const CMediaType *pMediaType)
{
    return IsOurFormat(pMediaType) ? S_OK : E_INVALIDARG;
}

HRESULT CVCamStream::DecideBufferSize(IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pProperties)
{
    CAutoLock cAutoLock(m_pFilter->pStateLock());
    VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER *) m_mt.Format();
    pProperties->cBuffers = 2;
    pProperties->cbBuffer = pvi->bmiHeader.biSizeImage;

    ALLOCATOR_PROPERTIES Actual;
    HRESULT hr = pAlloc->SetProperties(pProperties, &Actual);
    if (FAILED(hr)) return hr;
    if (Actual.cbBuffer < pProperties->cbBuffer) return E_FAIL;
    return NOERROR;
}

HRESULT CVCamStream::OnThreadCreate()
{
    m_streamStartQpc = 0;
    m_lastDeliverQpc = 0;
    m_lastFrameCounter = -1;
    if (!m_timer)
    {
        m_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_timer) m_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    m_reader.open();
    return NOERROR;
}

HRESULT CVCamStream::OnThreadDestroy()
{
    m_reader.close();
    return NOERROR;
}

//////////////////////////////////////////////////////////////////////////
//  IAMStreamConfig
//////////////////////////////////////////////////////////////////////////
HRESULT STDMETHODCALLTYPE CVCamStream::SetFormat(AM_MEDIA_TYPE *pmt)
{
    if (!pmt) return E_POINTER;
    if (!IsOurFormat(pmt)) return VFW_E_INVALIDMEDIATYPE;
    CAutoLock lock(m_pFilter->pStateLock());
    if (m_pFilter->IsActive()) return VFW_E_WRONG_STATE;
    m_mt = *pmt;
    IPin* pin = nullptr;
    ConnectedTo(&pin);
    if (pin)
    {
        IFilterGraph *pGraph = m_pParent->GetGraph();
        if (pGraph) pGraph->Reconnect(this);
        pin->Release();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CVCamStream::GetFormat(AM_MEDIA_TYPE **ppmt)
{
    if (!ppmt) return E_POINTER;
    *ppmt = CreateMediaType(&m_mt);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE CVCamStream::GetNumberOfCapabilities(int *piCount, int *piSize)
{
    if (!piCount || !piSize) return E_POINTER;
    *piCount = kVCamFormatCount;
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CVCamStream::GetStreamCaps(int iIndex, AM_MEDIA_TYPE **pmt, BYTE *pSCC)
{
    if (!pmt || !pSCC) return E_POINTER;
    if (iIndex < 0 || iIndex >= kVCamFormatCount) return S_FALSE;
    const int w = kVCamFormats[iIndex].width, h = kVCamFormats[iIndex].height;

    CMediaType mt;
    FillMediaType(&mt, w, h);
    *pmt = CreateMediaType(&mt);
    if (!*pmt) return E_OUTOFMEMORY;

    DECLARE_PTR(VIDEO_STREAM_CONFIG_CAPS, pvscc, pSCC);
    ZeroMemory(pvscc, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    pvscc->guid = FORMAT_VideoInfo;
    pvscc->VideoStandard = AnalogVideo_None;
    pvscc->InputSize.cx = w;
    pvscc->InputSize.cy = h;
    pvscc->MinCroppingSize.cx = w;
    pvscc->MinCroppingSize.cy = h;
    pvscc->MaxCroppingSize.cx = w;
    pvscc->MaxCroppingSize.cy = h;
    pvscc->MinOutputSize.cx = w;
    pvscc->MinOutputSize.cy = h;
    pvscc->MaxOutputSize.cx = w;
    pvscc->MaxOutputSize.cy = h;
    pvscc->MinFrameInterval = UNITS / kVCamFps;
    pvscc->MaxFrameInterval = UNITS / kVCamFps;
    pvscc->MinBitsPerSecond = w * h * 24 * kVCamFps;
    pvscc->MaxBitsPerSecond = w * h * 24 * kVCamFps;
    return S_OK;
}

//////////////////////////////////////////////////////////////////////////
// IKsPropertySet
//////////////////////////////////////////////////////////////////////////
HRESULT CVCamStream::Set(REFGUID guidPropSet, DWORD dwID, void *pInstanceData,
                        DWORD cbInstanceData, void *pPropData, DWORD cbPropData)
{
    return E_NOTIMPL;
}

HRESULT CVCamStream::Get(
    REFGUID guidPropSet,
    DWORD dwPropID,
    void *pInstanceData,
    DWORD cbInstanceData,
    void *pPropData,
    DWORD cbPropData,
    DWORD *pcbReturned
)
{
    if (guidPropSet != AMPROPSETID_Pin)             return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY)        return E_PROP_ID_UNSUPPORTED;
    if (pPropData == NULL && pcbReturned == NULL)   return E_POINTER;

    if (pcbReturned) *pcbReturned = sizeof(GUID);
    if (pPropData == NULL)          return S_OK;
    if (cbPropData < sizeof(GUID))  return E_UNEXPECTED;

    *(GUID *)pPropData = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

HRESULT CVCamStream::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD *pTypeSupport)
{
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}
