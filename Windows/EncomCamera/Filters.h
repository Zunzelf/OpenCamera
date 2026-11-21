#pragma once

#include <vector>
#include "../shared/FrameShm.h"

#define DECLARE_PTR(type, ptr, expr) type* ptr = (type*)(expr);

EXTERN_C const GUID CLSID_VirtualCam;

// Formats offered to applications. Index 0 is the default.
struct VCamFormat
{
    int width;
    int height;
};
static const VCamFormat kVCamFormats[] = {
    { 1280, 720 },
    { 1920, 1080 },
    { 640, 480 },
    { 640, 360 },
};
static const int kVCamFormatCount = sizeof(kVCamFormats) / sizeof(kVCamFormats[0]);
static const int kVCamFps = 30;

class CVCamStream;
class CVCam : public CSource
{
public:
    static CUnknown * WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT *phr);
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv);

    IFilterGraph *GetGraph() {return m_pGraph;}

private:
    CVCam(LPUNKNOWN lpunk, HRESULT *phr);
};

class CVCamStream : public CSourceStream, public IAMStreamConfig, public IKsPropertySet
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv);
    STDMETHODIMP_(ULONG) AddRef() { return GetOwner()->AddRef(); }
    STDMETHODIMP_(ULONG) Release() { return GetOwner()->Release(); }

    // IQualityControl
    STDMETHODIMP Notify(IBaseFilter * pSender, Quality q);

    // IAMStreamConfig
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE *pmt);
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE **ppmt);
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int *piCount, int *piSize);
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int iIndex, AM_MEDIA_TYPE **pmt, BYTE *pSCC);

    // IKsPropertySet
    HRESULT STDMETHODCALLTYPE Set(REFGUID guidPropSet, DWORD dwID, void *pInstanceData, DWORD cbInstanceData, void *pPropData, DWORD cbPropData);
    HRESULT STDMETHODCALLTYPE Get(REFGUID guidPropSet, DWORD dwPropID, void *pInstanceData,DWORD cbInstanceData, void *pPropData, DWORD cbPropData, DWORD *pcbReturned);
    HRESULT STDMETHODCALLTYPE QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD *pTypeSupport);

    // CSourceStream
    CVCamStream(HRESULT *phr, CVCam *pParent, LPCWSTR pPinName);
    ~CVCamStream();

    HRESULT FillBuffer(IMediaSample *pms);
    HRESULT DecideBufferSize(IMemAllocator *pIMemAlloc, ALLOCATOR_PROPERTIES *pProperties);
    HRESULT CheckMediaType(const CMediaType *pMediaType);
    HRESULT GetMediaType(int iPosition, CMediaType *pmt);
    HRESULT SetMediaType(const CMediaType *pmt);
    HRESULT OnThreadCreate(void);
    HRESULT OnThreadDestroy(void);

private:
    void waitForFrame(int64_t deadlineQpc);
    void sleepShort();
    const std::vector<uint8_t>& placeholder(int w, int h, bool appRunning);

    CVCam *m_pParent;
    ocam::FrameReader m_reader;
    int64_t m_lastOpenTryQpc = 0;
    int64_t m_lastFrameCounter = -1;
    int64_t m_streamStartQpc = 0;
    int64_t m_lastDeliverQpc = 0;
    HANDLE m_timer = NULL;
    std::vector<int> m_xmap;          // nearest-neighbour column lookup
    int m_xmapKey[4] = { 0, 0, 0, 0 };
    std::vector<uint8_t> m_placeholder;
    int m_placeholderKey[3] = { 0, 0, -1 };
};
