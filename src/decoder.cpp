#include "decoder.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kPrefetch = 3;          // frames decoded ahead of the last request
constexpr int kKeepBehind = 1;        // frames kept behind the last request
constexpr int kMaxForwardDecode = 45; // decode forward instead of seeking when the target is this close
constexpr size_t kMaxCache = kKeepBehind + 1 + kPrefetch;  // one 8K 10-bit frame is 34 MB
constexpr size_t kMaxSpare = 1;       // released frame buffers kept for the next frames

// An idle decoder closes its reader (this releases the hardware decoder and its video memory) and keeps
// only the last requested frame, so a paused clip still draws at once. Later it drops that frame too.
constexpr auto kCloseFront = std::chrono::seconds(20);
constexpr auto kCloseBackground = std::chrono::seconds(4);
constexpr auto kDropAfter = std::chrono::seconds(60);

std::atomic<uint64_t> g_Serial{0};

std::string hrStr(HRESULT hr)
{
    char b[32];
    snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}
} // namespace

struct StreamDecoder::Pool
{
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> spare;
    size_t cap = kMaxSpare;

    void setCap(size_t n)
    {
        std::lock_guard<std::mutex> lock(mutex);
        cap = n;
        if (spare.size() > cap) spare.resize(cap);
    }
};

struct StreamDecoder::Impl
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IMFDXGIDeviceManager> mgr;
    ComPtr<IMFSourceReader> reader;
    DWORD stream = 0;
    ComPtr<ID3D11Texture2D> staging;
    UINT stagingW = 0, stagingH = 0;
    UINT width = 0, height = 0;
    int bps = 1;  // bytes per sample of the negotiated output: 1 NV12, 2 P010
};

StreamDecoder::StreamDecoder(const std::wstring& path, uint32_t trackId, uint32_t firstSampleSize, int videoOrdinal,
                             int fpsNum, int fpsDen, int frameCount, int bitDepth)
    : m_Path(path), m_TrackId(trackId), m_FirstSampleSize(firstSampleSize), m_Ordinal(videoOrdinal), m_FpsNum(fpsNum), m_FpsDen(fpsDen), m_Frames(frameCount), m_BitDepth(bitDepth)
{
    m_Pool = std::make_shared<Pool>();
    m_Thread = std::thread([this] { run(); });
}

StreamDecoder::~StreamDecoder()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stop = true;
    }
    m_Cv.notify_all();
    if (m_Thread.joinable()) m_Thread.join();
}

void StreamDecoder::request(int frame, bool readAhead)
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_LastWanted = frame;
        m_ReadAhead = readAhead;
        m_LastRequest = std::chrono::steady_clock::now();
    }
    m_Cv.notify_all();
}

void StreamDecoder::setBackground(bool background)
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Background == background) return;
        m_Background = background;
    }
    m_Cv.notify_all();
}

std::shared_ptr<Nv12Frame> StreamDecoder::newFrame()
{
    // the deleter gives the buffer back to the pool: a new 34 MB allocation for each frame is slow
    std::shared_ptr<Nv12Frame> f(new Nv12Frame, [pool = m_Pool](Nv12Frame* p) {
        if (p->data.capacity())
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            if (pool->spare.size() < pool->cap) pool->spare.push_back(std::move(p->data));
        }
        delete p;
    });
    f->serial = ++g_Serial;
    return f;
}

std::shared_ptr<const Nv12Frame> StreamDecoder::get(int frame, bool readAhead)
{
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_LastWanted = frame;
    m_ReadAhead = readAhead;
    m_LastRequest = std::chrono::steady_clock::now();
    auto it = m_Cache.find(frame);
    if (it != m_Cache.end())
    {
        m_Cv.notify_all();  // let read-ahead continue
        return it->second;
    }
    m_Wanted.insert(frame);
    m_Cv.notify_all();
    bool ok = m_Cv.wait_for(lock, std::chrono::seconds(30), [&] {
        return m_Failed || m_Stop || m_Cache.count(frame) || frame >= m_Frames;
    });
    m_Wanted.erase(m_Wanted.find(frame));
    it = m_Cache.find(frame);
    if (it != m_Cache.end()) return it->second;
    if (!ok) m_Error = "timed out decoding frame " + std::to_string(frame);
    else if (frame >= m_Frames) m_Error = "frame " + std::to_string(frame) + " is past the end";
    return nullptr;
}

std::string StreamDecoder::error()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Error;
}

// Media Foundation neither keeps the MP4 track order nor exposes track IDs, so identify the stream by
// reading its first compressed sample and comparing the size with the MP4 sample table.
bool StreamDecoder::findStream(DWORD& streamOut)
{
    ComPtr<IMFSourceReader> probe;
    if (FAILED(MFCreateSourceReaderFromURL(m_Path.c_str(), nullptr, &probe))) return false;
    std::vector<DWORD> video;
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> t;
        HRESULT hr = probe->GetNativeMediaType(i, 0, &t);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        GUID major;
        if (SUCCEEDED(hr) && SUCCEEDED(t->GetMajorType(&major)) && major == MFMediaType_Video) video.push_back(i);
    }
    // MF may convert NAL length prefixes or prepend parameter sets, so pick the closest size rather than an exact match.
    long long bestDiff = -1;
    for (DWORD i : video)
    {
        probe->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        probe->SetStreamSelection(i, TRUE);
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = 0;
        probe->SetCurrentPosition(GUID_NULL, pv);
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        DWORD len = 0;
        if (SUCCEEDED(probe->ReadSample(i, 0, &idx, &flags, &ts, &sample)) && sample) sample->GetTotalLength(&len);
        logf("decoder: MF video stream %lu first sample %lu bytes (want %u for track %u)", i, len, m_FirstSampleSize, m_TrackId);
        long long diff = len ? std::llabs((long long)len - (long long)m_FirstSampleSize) : LLONG_MAX;
        if (len && (bestDiff < 0 || diff < bestDiff))
        {
            bestDiff = diff;
            streamOut = i;
        }
    }
    if (bestDiff >= 0) return true;
    if (m_Ordinal < int(video.size()))
    {
        logf("decoder: no size match for track %u, using video stream #%d", m_TrackId, m_Ordinal);
        streamOut = video[m_Ordinal];
        return true;
    }
    return false;
}

bool StreamDecoder::openReader()
{
    Impl& d = *m_Impl;
    HRESULT hr;

    // Prefer the NVIDIA adapter on hybrid-graphics laptops; otherwise the default adapter.
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset())
        {
            DXGI_ADAPTER_DESC1 desc;
            a->GetDesc1(&desc);
            if (desc.VendorId == 0x10DE) { adapter = a; break; }
        }
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2, D3D11_SDK_VERSION, &d.device, nullptr, &d.ctx);
    bool hw = SUCCEEDED(hr);
    UINT token = 0;
    if (hw)
    {
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(d.device.As(&mt))) mt->SetMultithreadProtected(TRUE);
        hw = SUCCEEDED(MFCreateDXGIDeviceManager(&token, &d.mgr)) && SUCCEEDED(d.mgr->ResetDevice(d.device.Get(), token));
    }
    if (!hw) logf("decoder: D3D11 device unavailable (%s), using software decode", hrStr(hr).c_str());

    for (int attempt = hw ? 0 : 1; attempt < 2; ++attempt)
    {
        bool useHw = attempt == 0;
        ComPtr<IMFAttributes> attr;
        MFCreateAttributes(&attr, 4);
        if (useHw)
        {
            attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, d.mgr.Get());
            attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }
        attr->SetUINT32(MF_LOW_LATENCY, TRUE);
        d.reader.Reset();
        hr = MFCreateSourceReaderFromURL(m_Path.c_str(), attr.Get(), &d.reader);
        if (FAILED(hr))
        {
            m_Error = "Media Foundation cannot open the file (" + hrStr(hr) + ")";
            return false;
        }
        if (m_StreamIndex < 0)
        {
            if (!findStream(d.stream))
            {
                m_Error = "video track " + std::to_string(m_TrackId) + " not found";
                return false;
            }
            m_StreamIndex = long(d.stream);
        }
        d.stream = DWORD(m_StreamIndex);
        d.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        d.reader->SetStreamSelection(d.stream, TRUE);
        // 10-bit streams decode to P010 so that no precision is lost; 8-bit streams decode to NV12
        hr = E_FAIL;
        for (int bps = m_BitDepth > 8 ? 2 : 1; bps >= 1 && FAILED(hr); --bps)
        {
            ComPtr<IMFMediaType> out;
            MFCreateMediaType(&out);
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, bps == 2 ? MFVideoFormat_P010 : MFVideoFormat_NV12);
            hr = d.reader->SetCurrentMediaType(d.stream, nullptr, out.Get());
            if (SUCCEEDED(hr)) d.bps = bps;
        }
        if (FAILED(hr))
        {
            logf("decoder: no %d-bit output available with %s decode (%s)", m_BitDepth, useHw ? "hardware" : "software", hrStr(hr).c_str());
            if (useHw) continue;
            m_Error = "no HEVC decoder available (install 'HEVC Video Extensions' from the Microsoft Store) " + hrStr(hr);
            return false;
        }
        ComPtr<IMFMediaType> cur;
        d.reader->GetCurrentMediaType(d.stream, &cur);
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &d.width, &d.height);
        m_Hardware = useHw;
        logf("decoder: opened stream %d (%ux%u %s) with %s decode", m_Ordinal, d.width, d.height, d.bps == 2 ? "P010" : "NV12",
             useHw ? "hardware" : "software");
        return true;
    }
    return false;
}

bool StreamDecoder::seek(int frame)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_I8;
    pv.hVal.QuadPart = (long long)(double(frame) * m_FpsDen * 1e7 / m_FpsNum);
    HRESULT hr = m_Impl->reader->SetCurrentPosition(GUID_NULL, pv);
    PropVariantClear(&pv);
    m_Next = -1;
    return SUCCEEDED(hr);
}

template <class F> std::shared_ptr<Nv12Frame> StreamDecoder::readFrame(bool& eos, F&& wantPixels)
{
    Impl& d = *m_Impl;
    eos = false;
    for (int tries = 0; tries < 64; ++tries)
    {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = d.reader->ReadSample(d.stream, 0, &idx, &flags, &ts, &sample);
        if (FAILED(hr))
        {
            m_Error = "ReadSample failed " + hrStr(hr);
            return nullptr;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            eos = true;
            return nullptr;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
        {
            ComPtr<IMFMediaType> cur;
            d.reader->GetCurrentMediaType(d.stream, &cur);
            MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &d.width, &d.height);
        }
        if (!sample) continue;

        auto f = newFrame();
        f->index = int(std::llround(double(ts) * m_FpsNum / (double(m_FpsDen) * 1e7)));
        if (!wantPixels(f->index)) return f;  // a frame on the way to a seek target: nobody will look at it
        f->width = int(d.width);
        f->height = int(d.height);
        auto alloc = [&](int bps) {
            f->bytesPerSample = bps;
            f->pitch = f->width * bps;
            size_t bytes = size_t(f->pitch) * f->height * 3 / 2;
            {
                std::lock_guard<std::mutex> lock(m_Pool->mutex);
                auto& spare = m_Pool->spare;
                for (size_t i = 0; i < spare.size(); ++i)
                    if (spare[i].capacity() >= bytes)
                    {
                        f->data = std::move(spare[i]);
                        spare.erase(spare.begin() + i);
                        break;
                    }
            }
            f->data.resize(bytes);
        };

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) continue;
        ComPtr<IMFDXGIBuffer> dx;
        if (SUCCEEDED(buf.As(&dx)))
        {
            ComPtr<ID3D11Texture2D> tex;
            UINT sub = 0;
            if (FAILED(dx->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dx->GetSubresourceIndex(&sub))) continue;
            D3D11_TEXTURE2D_DESC desc;
            tex->GetDesc(&desc);
            // the texture format is what the decoder really produced
            if (desc.Format == DXGI_FORMAT_P010 || desc.Format == DXGI_FORMAT_P016) alloc(2);
            else if (desc.Format == DXGI_FORMAT_NV12) alloc(1);
            else
            {
                m_Error = "unsupported decoder output format " + std::to_string(int(desc.Format));
                return nullptr;
            }
            if (!d.staging || d.stagingW != desc.Width || d.stagingH != desc.Height)
            {
                D3D11_TEXTURE2D_DESC sd = {};
                sd.Width = desc.Width;
                sd.Height = desc.Height;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                sd.Format = desc.Format;
                sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                d.staging.Reset();
                if (FAILED(d.device->CreateTexture2D(&sd, nullptr, &d.staging)))
                {
                    m_Error = "cannot create staging texture";
                    return nullptr;
                }
                d.stagingW = desc.Width;
                d.stagingH = desc.Height;
            }
            d.ctx->CopySubresourceRegion(d.staging.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
            D3D11_MAPPED_SUBRESOURCE map;
            if (FAILED(d.ctx->Map(d.staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
            {
                m_Error = "cannot map staging texture";
                return nullptr;
            }
            const uint8_t* src = static_cast<const uint8_t*>(map.pData);
            const uint8_t* srcUv = src + size_t(map.RowPitch) * desc.Height;
            uint8_t* dstUv = f->data.data() + size_t(f->pitch) * f->height;
            if (int(map.RowPitch) == f->pitch)
            {
                std::memcpy(f->data.data(), src, size_t(f->pitch) * f->height);
                std::memcpy(dstUv, srcUv, size_t(f->pitch) * (f->height / 2));
            }
            else
            {
                for (int y = 0; y < f->height; ++y)
                    std::memcpy(f->data.data() + size_t(y) * f->pitch, src + size_t(y) * map.RowPitch, f->pitch);
                for (int y = 0; y < f->height / 2; ++y)
                    std::memcpy(dstUv + size_t(y) * f->pitch, srcUv + size_t(y) * map.RowPitch, f->pitch);
            }
            d.ctx->Unmap(d.staging.Get(), 0);
        }
        else
        {
            alloc(d.bps);
            ComPtr<IMF2DBuffer> b2;
            BYTE* p = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch)))
            {
                for (int y = 0; y < f->height * 3 / 2; ++y)
                    std::memcpy(f->data.data() + size_t(y) * f->pitch, p + size_t(y) * pitch, f->pitch);
                b2->Unlock2D();
            }
            else
            {
                DWORD len = 0;
                if (FAILED(buf->Lock(&p, nullptr, &len))) continue;
                std::memcpy(f->data.data(), p, std::min<size_t>(len, f->data.size()));
                buf->Unlock();
            }
        }
        return f;
    }
    m_Error = "decoder produced no frames";
    return nullptr;
}

void StreamDecoder::run()
{
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    m_Impl = std::make_unique<Impl>();
    bool ok = openReader();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!ok)
        {
            m_Failed = true;
            logf("decoder: %s", m_Error.c_str());
        }
    }
    m_Cv.notify_all();
    m_Next = 0;
    int pendingSeek = -1;  // frame we seeked to; frames before it are skipped quickly
    int retryFrom = -1;    // where the last retry of an overshot seek started
    int seekBack = 60;     // how far before the target the next retry starts
    bool open = ok;        // false while the reader is closed because nobody asked for frames
    // m_LastRequest at the time the reader closed: read-ahead stays off until there is a new request
    auto quietSince = std::chrono::steady_clock::time_point::min();

    while (ok)
    {
        int target = -1;
        bool close = false;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            auto pick = [&] {
                target = -1;
                // explicit requests first, preferring ones ahead of the reader
                int best = INT_MAX, bestAhead = INT_MAX;
                for (int w : m_Wanted)
                {
                    if (m_Cache.count(w) || w >= m_Frames) continue;
                    best = std::min(best, w);
                    if (m_Next >= 0 && w >= m_Next) bestAhead = std::min(bestAhead, w);
                }
                if (bestAhead != INT_MAX) target = bestAhead;
                else if (best != INT_MAX) target = best;
                else if (m_LastWanted >= 0 && m_LastRequest != quietSince)
                    for (int f = m_LastWanted; f <= m_LastWanted + (m_ReadAhead ? kPrefetch : 0) && f < m_Frames; ++f)
                        if (!m_Cache.count(f)) { target = f; break; }
                return target >= 0;
            };
            while (!m_Stop && !pick())
            {
                auto now = std::chrono::steady_clock::now();
                auto closeAt = m_LastRequest + (m_Background ? kCloseBackground : kCloseFront);
                auto dropAt = m_LastRequest + kDropAfter;
                if (open && now >= closeAt)
                {
                    close = true;
                    break;
                }
                if (open) m_Cv.wait_until(lock, closeAt);
                else if (now < dropAt) m_Cv.wait_until(lock, dropAt);
                else
                {
                    m_Cache.clear();
                    m_Cv.wait(lock);
                }
            }
            if (m_Stop) break;
            if (close)
            {
                quietSince = m_LastRequest;
                for (auto it = m_Cache.begin(); it != m_Cache.end();)
                    it = it->first == m_LastWanted ? std::next(it) : m_Cache.erase(it);
            }
        }
        if (close)
        {
            m_Pool->setCap(0);
            m_Impl = std::make_unique<Impl>();
            open = false;
            continue;
        }
        if (!open)
        {
            m_Pool->setCap(kMaxSpare);
            if (!openReader())
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Failed = true;
                logf("decoder: %s", m_Error.c_str());
                m_Cv.notify_all();
                break;
            }
            open = true;
            m_Next = -1;
            pendingSeek = -1;
        }

        if (pendingSeek >= 0 && target >= pendingSeek && (m_Next < 0 || target - pendingSeek <= kMaxForwardDecode))
        {
            // still decoding towards an earlier seek target: keep going. The keyframe can be far before the
            // target (100 fps files have one per 100 frames), and a new seek would only land on it again.
        }
        else if (m_Next < 0 || target < m_Next || target - m_Next > kMaxForwardDecode)
        {
            if (!seek(target))
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Error = "seek failed";
                m_Failed = true;
                m_Cv.notify_all();
                break;
            }
            pendingSeek = target;
            retryFrom = -1;
            seekBack = 60;
        }

        bool eos = false;
        auto fr = readFrame(eos, [&](int index) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_Wanted.count(index) || (index >= m_LastWanted - kKeepBehind && index <= m_LastWanted + kPrefetch);
        });
        if (!fr)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (eos)
            {
                // nothing more to read: anything still wanted past this point does not exist
                m_Frames = std::min(m_Frames, std::max(0, m_Next));
                m_Next = INT_MAX / 2;
                pendingSeek = -1;
            }
            else
            {
                m_Failed = true;
                logf("decoder: %s", m_Error.c_str());
            }
            m_Cv.notify_all();
            if (!eos) break;
            continue;
        }
        if (pendingSeek >= 0 && fr->index > pendingSeek && m_Next < 0)
        {
            // the seek landed after the target (keyframe search overshot): back up, farther on each retry
            logf("decoder: seek to %d landed on %d, retrying %d earlier", pendingSeek, fr->index, seekBack);
            if (retryFrom == 0 || pendingSeek == 0) pendingSeek = -1;  // already started at the first frame
            else
            {
                retryFrom = std::max(0, pendingSeek - seekBack);
                seekBack *= 2;
                seek(retryFrom);
                continue;
            }
        }
        m_Next = fr->index + 1;
        if (pendingSeek >= 0 && fr->index >= pendingSeek) pendingSeek = -1;

        if (fr->data.empty()) continue;

        std::lock_guard<std::mutex> lock(m_Mutex);
        int lw = m_LastWanted;
        bool keep = m_Wanted.count(fr->index) || (fr->index >= lw - kKeepBehind && fr->index <= lw + kPrefetch);
        if (keep)
        {
            m_Cache[fr->index] = fr;
            while (m_Cache.size() > kMaxCache)
            {
                // evict the frame farthest from the last request that nobody is waiting on
                auto victim = m_Cache.end();
                int victimDist = -1;
                for (auto it = m_Cache.begin(); it != m_Cache.end(); ++it)
                {
                    if (m_Wanted.count(it->first)) continue;
                    int dist = std::abs(it->first - lw) + (it->first < lw ? 1000 : 0);
                    if (dist > victimDist) { victimDist = dist; victim = it; }
                }
                if (victim == m_Cache.end()) break;
                m_Cache.erase(victim);
            }
        }
        m_Cv.notify_all();
    }

    m_Impl.reset();
    MFShutdown();
    if (SUCCEEDED(hrCo)) CoUninitialize();
}
